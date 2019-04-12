#include "rtc_base/scoped_ref_ptr.h"
#include "rtc_base/ref_count.h"
#include "rtc_base/ref_counted_object.h"
#include <string>
#include <iostream>
#include <utility>

namespace
{
	class OperationStatusObserver : public rtc::RefCountInterface
	{
	public:
		virtual void OnSuccess() = 0;
		virtual void OnFailure(const std::string& error) = 0;
	protected:
		~OperationStatusObserver() override = default;
	};

	class MyOpStatusObserver : public OperationStatusObserver
	{
	public:
		static OperationStatusObserver* Create() {
			return new rtc::RefCountedObject<MyOpStatusObserver>();
		}

		void OnSuccess() override {
			std::cout << "Successo\n";
		}

		virtual void OnFailure(const std::string& error) override {
			std::cout << "Errore: " << error << '\n';
		}
	};

	class MySubject
	{
	public:
		explicit MySubject(OperationStatusObserver* observer)
		: observer_(observer) {}

		void FailingMethod() {
			observer_->OnFailure("chiamata al metodo che fallisce");
		}

		void SuccessfulMethod() {
			observer_->OnSuccess();
		}

	private:
		rtc::scoped_refptr<OperationStatusObserver> observer_;
	};
}

void SampleObserver()
{
	MySubject subj(MyOpStatusObserver::Create());
	subj.SuccessfulMethod();
	subj.FailingMethod();
}