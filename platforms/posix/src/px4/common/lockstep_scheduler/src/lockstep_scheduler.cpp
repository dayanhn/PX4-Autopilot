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
set_absolute_time ����ͨ���������³�����
?����ʱ��ͬ����
�� Gazebo �����У�������ʱ��ͬ���� PX4 �� LockstepScheduler��
?������ȣ�
֪ͨ���еȴ�ĳ��ʱ����������ִ�С�
?�¼�������
������ʱ����ص��¼����綨ʱ����ʱ����

������̣������ڲ���ʱ�����������������������ɵ�������������Ƴ���֪ͨ�����Ѿ���ʱ������
*/
void LockstepScheduler::set_absolute_time(uint64_t time_us)
{
	if (_time_us == 0 && time_us > 0) {
		PX4_INFO("setting initial absolute time to %" PRIu64 " us", time_us);
	}

	_time_us = time_us;//����ǰʱ�� _time_us ����Ϊ�����ʱ�� time_us

	{
		std::unique_lock<std::mutex> lock_timed_waits(_timed_waits_mutex);
		_setting_time = true;
		/*���� _timed_waits ���������Ѿ���ɵ����� */
		TimedWait *timed_wait = _timed_waits;//_timed_waits��һ�������洢���еȴ�ʱ�������
		TimedWait *timed_wait_prev = nullptr;

		while (timed_wait) {
			// Clean up the ones that are already done from last iteration.
			if (timed_wait->done) {//��������Ѿ���ɣ�����������Ƴ�
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
			//�������ĵȴ�ʱ�� timed_wait->time_us С�ڵ��ڵ�ǰʱ�� time_us����������δ��ʱ���򱾴��Ѿ���ʱ
			//(��ǰʱ���Ѿ����������õ�ʱ��)����Ҫ���Ѹ�����
			if (timed_wait->time_us <= time_us &&
			    !timed_wait->timeout) {
				// We are abusing the condition here to signal that the time
				// has passed.
				pthread_mutex_lock(timed_wait->passed_lock);
				timed_wait->timeout = true;
				pthread_cond_broadcast(timed_wait->passed_cond);//ͨ�����������㲥�������еȴ�������
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
	//����һ���ֲ߳̾��� TimedWait �������ڴ洢��ǰ����ĵȴ���Ϣ��
	static thread_local TimedWait timed_wait;
	{
		std::lock_guard<std::mutex> lock_timed_waits(_timed_waits_mutex);

		// The time has already passed.
		//�����ǰʱ�� _time_us �Ѿ����ڵ��ڵȴ�ʱ�� time_us����ֱ�ӷ��س�ʱ,���ⲻ��Ҫ�ĵȴ���
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
	//�ͷ� lock ����������������ǰ�̣߳�ֱ����������������
	int result = pthread_cond_wait(cond, lock);
	//ִ�е������ʾ�߳��Ѿ���������
	const bool timeout = timed_wait.timeout;
	//����������������ѣ��������Ѿ���ʱ��timed_wait.timeout Ϊ true�����򷵻س�ʱ���� ETIMEDOUT
	if (result == 0 && timeout) {
		result = ETIMEDOUT;
	}
	//�������Ϊ����ɣ����� set_absolute_time �������������
	timed_wait.done = true;
	//���û�г�ʱ��Ҳ����˵����set_absolute_time���ѵģ�������̼߳���ִ�в����أ���Lock��cond����������
	//����set_absolute_timeҲ����ִ�У����Ҹմ��������������������timed_wait->done��־��
	//���ұ������timed_wait.done��û����Ϊtrue�����Ҹպó�ʱ����set_absolute_time�ͻ��ȡ�������������������Ϣ
	//������������Ѿ����������Ӷ�����������Ϊ�˱������������������Ҫ��һ��set_absolute_time���������뷢������Ϣ��
	//��ô���������ͷ����������л����ȡ��Ȼ���ȡ_timed_waits_mutex����֤set_absolute_time�Ѿ�ִ���꣬Ȼ�����ͷ�
	//Ȼ���ȡ���������������ִ�С�����set_absolute_time�ٴ���ʱ��������timed_wait->done=true��־Ҳ�ǰ�ȫ�ġ�
	//����ǳ�ʱ���϶���set_absolute_time����������Ϣ�����ģ�������Ҳ��������Ҫ��������������������������Ҫ���⴦��
	//��֮Ҫ������ǲ�Ҫ��set_absolute_time�л���ȥ��ȡ����������Ѿ���������
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
