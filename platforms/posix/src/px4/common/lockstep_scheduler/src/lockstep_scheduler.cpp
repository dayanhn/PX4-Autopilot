/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <lockstep_scheduler/lockstep_scheduler.h>

#include <px4_platform_common/log.h>

LockstepScheduler::~LockstepScheduler()
{
	// cleanup the linked list
	std::unique_lock<std::mutex> lock_timed_waits(_timed_waits_mutex);

	while (_timed_waits) {
		TimedWait *tmp = _timed_waits;
		_timed_waits = _timed_waits->next;
		tmp->removed = true;
	}
}
/*
set_absolute_time 函数通常用于以下场景：
?仿真时间同步：
在 Gazebo 仿真中，将仿真时间同步到 PX4 的 LockstepScheduler。
?任务调度：
通知所有等待某个时间的任务继续执行。
?事件驱动：
处理与时间相关的事件（如定时器超时）。

处理过程：更新内部的时间戳，处理任务链表，对已完成的任务从链表中移除，通知所有已经超时的任务
*/
void LockstepScheduler::set_absolute_time(uint64_t time_us)
{
	if (_time_us == 0 && time_us > 0) {
		PX4_INFO("setting initial absolute time to %" PRIu64 " us", time_us);
	}

	_time_us = time_us;//将当前时间 _time_us 更新为传入的时间 time_us

	{
		std::unique_lock<std::mutex> lock_timed_waits(_timed_waits_mutex);
		_setting_time = true;
		/*遍历 _timed_waits 链表，清理已经完成的任务 */
		TimedWait *timed_wait = _timed_waits;//_timed_waits是一个链表，存储所有等待时间的任务
		TimedWait *timed_wait_prev = nullptr;

		while (timed_wait) {
			// Clean up the ones that are already done from last iteration.
			if (timed_wait->done) {//如果任务已经完成，则从链表中移除
				// Erase from the linked list
				if (timed_wait_prev) {
					timed_wait_prev->next = timed_wait->next;

				} else {
					_timed_waits = timed_wait->next;
				}

				TimedWait *tmp = timed_wait;
				timed_wait = timed_wait->next;
				tmp->removed = true;
				continue;
			}
			//如果任务的等待时间 timed_wait->time_us 小于等于当前时间 time_us，且任务尚未超时，则本次已经超时
			//(当前时间已经超过了设置的时间)，需要唤醒该任务
			if (timed_wait->time_us <= time_us &&
			    !timed_wait->timeout) {
				// We are abusing the condition here to signal that the time
				// has passed.
				pthread_mutex_lock(timed_wait->passed_lock);
				timed_wait->timeout = true;
				pthread_cond_broadcast(timed_wait->passed_cond);//通过条件变量广播唤醒所有等待的任务
				pthread_mutex_unlock(timed_wait->passed_lock);
			}

			timed_wait_prev = timed_wait;
			timed_wait = timed_wait->next;
		}

		_setting_time = false;
	}
}

int LockstepScheduler::cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *lock, uint64_t time_us)
{
	// A TimedWait object might still be in timed_waits_ after we return, so its lifetime needs to be
	// longer. And using thread_local is more efficient than malloc.
	//定义一个线程局部的 TimedWait 对象，用于存储当前任务的等待信息。
	static thread_local TimedWait timed_wait;
	{
		std::lock_guard<std::mutex> lock_timed_waits(_timed_waits_mutex);

		// The time has already passed.
		//如果当前时间 _time_us 已经大于等于等待时间 time_us，则直接返回超时,避免不必要的等待。
		if (time_us <= _time_us) {
			return ETIMEDOUT;
		}

		timed_wait.time_us = time_us;
		timed_wait.passed_cond = cond;
		timed_wait.passed_lock = lock;
		timed_wait.timeout = false;
		timed_wait.done = false;

		// Add to linked list if not removed yet (otherwise just re-use the object)
		if (timed_wait.removed) {
			timed_wait.removed = false;
			timed_wait.next = _timed_waits;
			_timed_waits = &timed_wait;
		}
	}
	//释放 lock 互斥锁，并阻塞当前线程，直到条件变量被唤醒
	int result = pthread_cond_wait(cond, lock);
	//执行到这里表示线程已经被唤醒了
	const bool timeout = timed_wait.timeout;
	//如果条件变量被唤醒，但任务已经超时（timed_wait.timeout 为 true），则返回超时错误 ETIMEDOUT
	if (result == 0 && timeout) {
		result = ETIMEDOUT;
	}
	//标记任务为已完成，允许 set_absolute_time 函数清理该任务
	timed_wait.done = true;
	//如果没有超时，也就是说不是set_absolute_time唤醒的，如果本线程继续执行并返回，则Lock和cond都会析构。
	//假设set_absolute_time也正在执行，并且刚处理完链表中所有任务的timed_wait->done标志，
	//并且本任务的timed_wait.done还没有置为true，而且刚好超时，那set_absolute_time就会获取本任务的锁发送条件消息
	//而本任务的锁已经被析构，从而发生死锁。为了避免这种情况，我们需要等一等set_absolute_time。假设它想发条件消息，
	//那么本任务先释放锁，让它有机会获取，然后获取_timed_waits_mutex，保证set_absolute_time已经执行完，然后再释放
	//然后获取本任务的锁，继续执行。后续set_absolute_time再触发时看到的是timed_wait->done=true标志也是安全的。
	//如果是超时，肯定是set_absolute_time发的条件消息触发的，它后续也不会则需要本任务的锁，所以这种情况不需要特殊处理
	//总之要避免的是不要让set_absolute_time有机会去获取本任务可能已经析构的锁
	if (!timeout && _setting_time) {
		// This is where it gets tricky: the timeout has not been triggered yet,
		// and another thread is in set_absolute_time().
		// If it already passed the 'done' check, it will access the mutex and
		// the condition variable next. However they might be invalid as soon as we
		// return here, so we wait until set_absolute_time() is done.
		// In addition we have to unlock 'lock', otherwise we risk a
		// deadlock due to a different locking order in set_absolute_time().
		// Note that this case does not happen too frequently, and thus can be
		// a bit more expensive.
		pthread_mutex_unlock(lock);
		_timed_waits_mutex.lock();
		_timed_waits_mutex.unlock();
		pthread_mutex_lock(lock);
	}

	return result;
}

int LockstepScheduler::usleep_until(uint64_t time_us)
{
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

	pthread_mutex_lock(&lock);

	int result = cond_timedwait(&cond, &lock, time_us);

	if (result == ETIMEDOUT) {
		// This is expected because we never notified to the condition.
		result = 0;
	}

	pthread_mutex_unlock(&lock);

	return result;
}
