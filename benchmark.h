#pragma once

#include "papi.h"
#include "chrono.h"
#include "printers.h"

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>

#include <boost/fusion/adapted/std_tuple.hpp>
#include <boost/fusion/algorithm/iteration/for_each.hpp>

#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <tuple>

namespace benchmark
{

struct test_report
{
	test_report(long iterations, int64_t cycles)
	: m_iterations(iterations), m_cycles(cycles) {}

	long    iteration_count() const { return m_iterations; }
	int64_t total_cycles() const { return m_cycles; }

	double  cycles_per_task() const { return m_cycles / (double)m_iterations; }
	std::chrono::nanoseconds time_per_task() const { return chrono::from_cycles(cycles_per_task()); }

private:
	long    m_iterations;
	int64_t m_cycles;
};

struct test_base
{
	test_base(const std::string& name)
	 : m_name(name) {}
	virtual ~test_base() {}

	virtual test_report run() const =0;

	const std::string& name() const { return m_name; }

private:
	std::string m_name;
};

template <typename _CallableT, typename... _PAPIWrappersT>
struct test : public test_base
{
	test(const std::string& name, _CallableT&& callable = _CallableT())
	 : test_base(name),
	   m_callable(callable) {}

	test_report run() const override
	{
		std::tuple<_PAPIWrappersT...> papi_wrappers;

		using namespace boost::accumulators;
		accumulator_set<int64_t, stats<tag::mean>> acc;

		chrono sampling_chrono;
		sampling_chrono.start();
		int64_t elapsed = 0;

		constexpr std::chrono::milliseconds max_sampling_time(100);

		chrono c;

		for (int i = 0; i < 1e3 && sampling_chrono.elapsed_time() < max_sampling_time; ++i)
		{
			c.start();
			m_callable();
			acc(c.elapsed());
		}

		long iterations = std::chrono::seconds(1) / chrono::from_cycles(mean(acc));

		auto run_benchmark = [&]() -> auto
		{
			c.restart();

			for (long i = 0; i < iterations; ++i)
				m_callable();

			return c.elapsed();
		};

		constexpr long papi_wrapppers_count = static_cast<long>(std::tuple_size<decltype(papi_wrappers)>::value);

		if (papi_wrapppers_count == 0)
		{
			int64_t cycles = run_benchmark();
			return {iterations, cycles};
		}

		int64_t cycles = 0;

		boost::fusion::for_each(papi_wrappers, [&](auto& papi)
		{
			papi.start();
			cycles += run_benchmark();
			papi.stop();
		});

		return {iterations * papi_wrapppers_count, cycles};
	}

private:
	_CallableT m_callable;
};

struct suite_report
{
	std::vector<std::pair<std::string, test_report>> tests;
};

struct suite_base
{
	virtual ~suite_base() {}

	virtual std::vector<std::reference_wrapper<const std::string>> test_names() const =0;
};

template <typename... _PAPIWrappersT>
struct suite : public suite_base
{
	typedef std::function<void(const std::string&, const test_report&)> test_complete_t;
	typedef std::function<void(const suite_report&)> suite_complete_t;

	template <typename _CallableT>
	suite& add(const std::string& name,
			   _CallableT&& callable)
	{
		m_tests.emplace_back(new test<_CallableT, _PAPIWrappersT...>(name, std::move(callable)));
		return *this;
	}

	template <typename _PrinterT>
	suite& set_printer(_PrinterT&& printer = _PrinterT())
	{
		m_printer.reset(new _PrinterT(std::move(printer)));
		return *this;
	}

	std::vector<std::reference_wrapper<const std::string>> test_names() const override
	{
		std::vector<std::reference_wrapper<const std::string>> v;

		for (const auto& test : m_tests)
			v.push_back(test->name());

		return v;
	}

	/*
	auto papi_counter_names() const
	{
		std::vector<std::reference_wrapper<const std::string>> v;

		for (const auto& papi_wrapper : m_papi_wrappers)
			v.push_back(papi_wrapper->name());

		return v;
	}
	*/

	suite& on_test_complete(test_complete_t f)
	{
		m_on_test_complete = f;
		return *this;
	}

	suite& on_complete(suite_complete_t f)
	{
		m_on_complete = f;
		return *this;
	}

	suite& run()
	{
		suite_report r;

		if (m_printer)
			m_printer->on_start(*this);

		for (const auto& p : m_tests)
		{
			test_report test_report = p->run();
			r.tests.emplace_back(p->name(), test_report);

			if (m_on_test_complete)
				m_on_test_complete(p->name(), test_report);

			if (m_printer)
				m_printer->on_test_complete(p->name(), test_report);
		}

		if (m_on_complete)
			m_on_complete(r);

		return *this;
	}

	std::vector<std::unique_ptr<test_base>> m_tests;
	std::unique_ptr<printer_base> m_printer;

	test_complete_t m_on_test_complete;
	suite_complete_t m_on_complete;
};

}
