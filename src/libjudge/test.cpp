#include <memory>
#include <string>
#include <cstring>
#include <cassert>
#include <utility>
#include <algorithm>
#include <winstl/synch/event.hpp>
#include <judgefs.h>
#include "test.hpp"
#include "testcase.hpp"
#include "pool.hpp"
#include "compiler.hpp"
#include "util.hpp"

using namespace std;
using winstl::event;

namespace judge {

struct test::context {
	explicit context(const std::shared_ptr<testcase_impl> &testcase)
		: event(true, false)
		, status(JSTATUS_SUCCESS)
		, testcase(testcase)
		, result()
	{}

	winstl::event event;
	std::shared_ptr<testcase_impl> testcase;
	jstatus_t status;
	judge_result result;
};


test::test(const bool spj, const shared_ptr<pool> &pool,
	const shared_ptr<compiler> &compiler,
	judgefs *source_fs, const string &source_path)
	: spj_(spj)
	, pool_(pool)
	, compiler_(compiler)
	, source_fs_(source_fs)
	, source_path_(source_path)
	, phase_(JUDGE_PHASE_NO_MORE)
{
	memset(&summary_result_, 0, sizeof(summary_result_));
	judgefs_add_ref(source_fs_);
}

test::~test()
{
	judgefs_release(source_fs_);
}

void test::add(const shared_ptr<testcase_impl> &testcase)
{
	contexts_.push_back(make_shared<context>(testcase));
}

void test::step()
{
	if (spj_) {
		test::step_spj();
	} else {
		test::step_normal();
	}
}

void test::step_spj()
{
	switch (phase_) {
	case JUDGE_PHASE_NO_MORE:
		phase_ = JUDGE_PHASE_COMPILE;
		index_ = 0;
		last_result_ = judge_result();
		last_result_.flag = JUDGE_ACCEPTED;
		last_result_.time_usage_ms = 0;
		last_result_.memory_usage_kb = 0;
		last_result_.runtime_error = 0;
		last_result_.judge_output = "";
		last_result_.user_output = nullptr;
		last_context_.reset();
		return;

	case JUDGE_PHASE_COMPILE:
		phase_ = JUDGE_PHASE_TESTCASE;
		index_ = 0;
		break;

	case JUDGE_PHASE_TESTCASE:
		++index_;
		break;

	default:
		phase_ = JUDGE_PHASE_NO_MORE;
		index_ = 0;
		last_result_ = judge_result();
		last_context_.reset();
		return;
	}

	auto compiler = compiler_;
	auto source_fs = source_fs_;
	auto source_path = source_path_;
	auto pool = pool_;

	assert(phase_ == JUDGE_PHASE_TESTCASE);

	if (!contexts_.empty()) {
		if (index_ == 0) {
			for_each(contexts_.begin(), contexts_.end(),
				[&](const shared_ptr<context> &ctx)->void
			{
				pool_->thread_pool().queue([pool, ctx, &compiler, &source_fs, &source_path]()->void {
					//auto &env0 = env;
					auto &ctx0 = ctx;
					ctx->status = util::wrap([&]()->jstatus_t {
						auto cr = ctx0->testcase->compile_spj(*pool, compiler, source_fs, source_path);
						auto result = judge_result();
						if (cr->flag != JUDGE_ACCEPTED) {
							result.flag = cr->flag;
						} else if (cr->flag == JUDGE_ACCEPTED && cr->exit_code != 0) {
							result.flag = JUDGE_COMPILE_ERROR;
							result.judge_output = cr->std_output.c_str();
						} else {
							result.flag = JUDGE_ACCEPTED;
						}
						if (cr->flag == JUDGE_ACCEPTED && cr->exit_code == 0) {
							vector<shared_ptr<env> > envs;
							pool->take_env(back_inserter(envs), 1);
							shared_ptr<env> env = move(envs.front());
							result = ctx0->testcase->run(*env, *cr);
						}
						ctx0->result = result; 
						return JSTATUS_SUCCESS;
					});
					ctx->event.set();
				});
			});
		}

		last_context_ = contexts_.front();
		contexts_.pop_front();
		if (::WaitForSingleObject(last_context_->event.get(), INFINITE) == WAIT_FAILED) {
			throw win32_exception(::GetLastError());
		}
		if (!JSUCCESS(last_context_->status)) {
			throw judge_exception(last_context_->status);
		}
		last_result_ = last_context_->result;
		summary_result_.flag = max(summary_result_.flag, last_result_.flag);
		summary_result_.time_usage_ms += last_result_.time_usage_ms;
		summary_result_.memory_usage_kb = max(summary_result_.memory_usage_kb, last_result_.memory_usage_kb);
		return;
	}

	phase_ = JUDGE_PHASE_SUMMARY;
	index_ = 0;
	last_result_ = summary_result_;
	last_context_.reset();
}

void test::step_normal()
{
	switch (phase_) {
	case JUDGE_PHASE_NO_MORE:
		phase_ = JUDGE_PHASE_COMPILE;
		index_ = 0;
		compiler_result_ = compiler_->compile_normal(*pool_, source_fs_, source_path_);
		last_result_ = judge_result();
		if (compiler_result_->flag == JUDGE_ACCEPTED) {
			if (compiler_result_->exit_code == 0) {
				last_result_.flag = JUDGE_ACCEPTED;
			} else {
				last_result_.flag = JUDGE_COMPILE_ERROR;
			}
		} else {
			last_result_.flag = compiler_result_->flag;
		}
		last_result_.time_usage_ms = compiler_result_->time_usage_ms;
		last_result_.memory_usage_kb = compiler_result_->memory_usage_kb;
		last_result_.runtime_error = compiler_result_->runtime_error;
		last_result_.judge_output = compiler_result_->std_output.c_str();
		last_result_.user_output = nullptr;
		last_context_.reset();
		return;

	case JUDGE_PHASE_COMPILE:
		phase_ = JUDGE_PHASE_TESTCASE;
		index_ = 0;
		break;

	case JUDGE_PHASE_TESTCASE:
		++index_;
		break;

	default:
		phase_ = JUDGE_PHASE_NO_MORE;
		index_ = 0;
		last_result_ = judge_result();
		last_context_.reset();
		return;
	}

	assert(phase_ == JUDGE_PHASE_TESTCASE);
	assert(compiler_result_);

	if (compiler_result_->flag != JUDGE_ACCEPTED || compiler_result_->exit_code != 0) {
		summary_result_.flag = max(summary_result_.flag, JUDGE_COMPILE_ERROR);
	} else if (!contexts_.empty()) {
		if (index_ == 0) {
			for_each(contexts_.begin(), contexts_.end(),
				[&](const shared_ptr<context> &ctx)->void
			{
				vector<shared_ptr<env> > envs;
				pool_->take_env(back_inserter(envs), 1);
				shared_ptr<env> env = move(envs.front());
				shared_ptr<compiler::result> cr = compiler_result_;
				pool_->thread_pool().queue([env, ctx, cr]()->void {
					auto &env0 = env;
					auto &ctx0 = ctx;
					auto &cr0 = cr;
					ctx->status = util::wrap([&env0, &ctx0, &cr0]()->jstatus_t {
						ctx0->result = ctx0->testcase->run(*env0, *cr0);
						return JSTATUS_SUCCESS;
					});
					ctx->event.set();
				});
			});
		}

		last_context_ = contexts_.front();
		contexts_.pop_front();
		if (::WaitForSingleObject(last_context_->event.get(), INFINITE) == WAIT_FAILED) {
			throw win32_exception(::GetLastError());
		}
		if (!JSUCCESS(last_context_->status)) {
			throw judge_exception(last_context_->status);
		}
		last_result_ = last_context_->result;
		summary_result_.flag = max(summary_result_.flag, last_result_.flag);
		summary_result_.time_usage_ms += last_result_.time_usage_ms;
		summary_result_.memory_usage_kb = max(summary_result_.memory_usage_kb, last_result_.memory_usage_kb);
		return;
	}

	phase_ = JUDGE_PHASE_SUMMARY;
	index_ = 0;
	last_result_ = summary_result_;
	last_context_.reset();
}

uint32_t test::phase()
{
	return phase_;
}

uint32_t test::index()
{
	return index_;
}

const judge_result &test::last_result()
{
	return last_result_;
}

}
