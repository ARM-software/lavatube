#include "rangetracking.h"

#include "tests/tests.h"

static void test_exposure()
{
	exposure r;
	exposure r2;
	assert(r.list().size() == 0);
	assert(r.bytes() == 0);
	assert(r.overlap(r2).last == 0);
	assert(r.overlap(r2).first == 0);
	assert(r2.list().size() == 0);
	r.self_test();

	assert((r2.fetch(3, 7, false) == range{0, 0}));
	assert((r2.fetch(3, 7, true) == range{0, 0}));

	r.add_os(0, 512);
	r.add_os(512, 512);
	r.add_os(1024, 512);
	r.add_os(1536, 512);
	r.add_os(2048, 512);
	assert(r.bytes() == 2048 + 512);
	assert((r.span() == range{ 0, 2048 + 512 - 1 }));
	r.self_test();
	r.clear();

	r.add_os(0, 11);
	r2.add(3, 5);
	assert((r.overlap(r2) == range{3,5}));
	assert((r2.overlap(r) == range{3,5}));
	r.self_test();
	r2.self_test();
	assert(r.list().size() == 1);
	assert(r.bytes() == 11);
	assert((r.fetch(0, 10, false) == range{0, 10}));
	r.self_test();
	assert(r.bytes() == 0);
	assert(r.size() == 0);
	r.self_test();
	r.clear();
	r2.clear();

	r.add_os(3, 2);
	r2.add_os(0, 7);
	assert((r.overlap(r2) == range{3,4}));
	r.self_test();
	assert(r.list().size() == 1);
	assert(r.bytes() == 2);
	assert((r.fetch(0, 10, false) == range{3, 4}));
	r.self_test();
	assert(r.bytes() == 0);
	assert(r.size() == 0);
	r.self_test();
	r.clear();
	r2.clear();

	r.add(0, 10);
	r2.add(11, 15);
	assert((r.overlap(r2) == range{0,0}));
	r.self_test();
	assert(r.list().size() == 1);
	assert(r.bytes() == 11);
	assert((r.fetch(0, 10, true) == range{0, 10}));
	r.self_test();
	r.add(5, 10);
	r.self_test();
	r.add(10, 10);
	r.self_test();
	r.add(5, 6);
	r.self_test();
	assert(r.size() == 1);
	assert(r.bytes() == 11);
	r.clear();
	r.self_test();
	r2.clear();

	r.add(3, 7);
	r.self_test();
	assert(r.list().size() == 1);
	assert((r.fetch(5, 6, false) == range{5, 6})); // should result in two ranges of 3-4 and 7-7
	r.self_test();
	assert(r.list().size() == 2);
	assert(r.bytes() == 3);
	assert((r.fetch(3, 7, true) == range{3,7}));
	r.self_test();
	assert(r.list().size() == 2);
	assert((r.fetch(3, 7, false) == range{3,7}));
	r.self_test();
	assert(r.list().size() == 0);
	r.clear();

	r.add(0, 1);
	r.self_test();
	assert(r.bytes() == 2);
	assert(r.list().size() == 1);
	assert((r.fetch(2, 3, false) == range{0, 0}));
	assert((r.fetch(2, 3, true) == range{0, 0}));
	r.self_test();
	r.add(9, 10);
	assert(r.list().size() == 2);
	assert((r.fetch(2, 3, false) == range{0, 0}));
	r.add(9, 10);
	assert(r.list().size() == 2);
	r.self_test();
	r.clear();

	r.add(0, 327679);
	range e { 294912, 327679 };
	range w = r.fetch(e, false);
	assert((w == range{ 294912, 327679 }));
	assert((r.span() == range{ 0, 294911 }));
	r.self_test();
	r.clear();

	r.add(0, 2097151);
	r.add(3145728, 14680063);
	r.fetch(3145728, 4194303, false);
	r.self_test();
	r.clear();

	// high fragmentation test
	for (int i = 0; i <= 1000; i++) r.add(i * 4, i * 4 + 2);
	r.self_test();
	assert(r.span().first == 0);
	assert(r.span().last == 4002);
	for (int i = 0; i <= 1000; i++) r.add(i * 4 + 1, i * 4 + 3);
	assert(r.size() == 1); // should merge all of them into one
	assert(r.span().first == 0);
	assert(r.span().last == 4003);
	r.self_test();
	// refragment
	for (int i = 0; i <= 1000; i++) r.fetch(i * 3, i * 3 + 1, false);
	assert(r.size() > 1);
	r.self_test();
	for (int i = 0; i <= 1000; i++) r.add(i * 3, i * 3 + 1);
	assert(r.size() == 1); // back to one
	r.self_test();
	r.clear();
}

int main()
{
	test_exposure();
	return 0;
}
