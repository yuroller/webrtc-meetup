#include "rtc_base/thread.h"
#include "rtc_base/event.h"
#include "rtc_base/message_handler.h"

#include <functional>
#include <iostream>
#include <thread>

namespace
{
	class SignalerExplicit : public rtc::MessageHandler
	{
	public:
		explicit SignalerExplicit(rtc::Event* finished)
			: finished_(finished) {}

		void OnMessage(rtc::Message* pmsg) override {
			std::cout << "signalerExplicit on thread "
				<< std::this_thread::get_id() << '\n';
			std::cout << "from: " << pmsg->posted_from.ToString()
				<< " id: " << pmsg->message_id << '\n';
			finished_->Set();
		}

	private:
		rtc::Event* finished_;
	};
}

void SampleThread()
{
	rtc::Event finished(true, false);
	//rtc::FunctorMessageHandler<void, std::function<void()>> signaler(
	//	[&finished] {
	//	std::cout << "signaler on thread "
	//      << std::this_thread::get_id() << '\n';
	//	finished.Set();
	//});

	SignalerExplicit signaler(&finished);

	std::cout << "main on thread "
		<< std::this_thread::get_id() << '\n';

	auto t1 = rtc::Thread::Create();
	t1->SetName("t1", nullptr);
	t1->Start();

	t1->Post(RTC_FROM_HERE, nullptr, 55);
	rtc::Message msg;
	if (t1->Get(&msg, 0)) {
		std::cout << "ricevuto " << msg.message_id << '\n';
	}
	else {
		std::cerr << "errore: nessun messaggio\n";
	}
	//t1->Post(RTC_FROM_HERE, &signaler);
	t1->PostDelayed(RTC_FROM_HERE, 5000, &signaler, 783);
	int ret = t1->Invoke<int>(RTC_FROM_HERE, []() { // this is sync
		std::cout << "invoked synchronously on thread "
			<< std::this_thread::get_id() << '\n';
		return 42;
	});
	std::cout << "returned " << ret << '\n';

	finished.Wait(rtc::Event::kForever);
}
