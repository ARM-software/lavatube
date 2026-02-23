#include <thread>
#include "lavatube.h"

#include "tests/tests.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

static change_source make_source(uint32_t frame)
{
	change_source s;
	s.call = frame + 1;
	s.frame = frame;
	s.thread = 1;
	s.call_id = 1;
	return s;
}

static bool same_source(const change_source& a, const change_source& b)
{
	return a.call == b.call && a.frame == b.frame && a.thread == b.thread && a.call_id == b.call_id;
}

static void test_contiguous_basic()
{
	host_write_regions regions;
	change_source a = make_source(1);
	regions.register_source(100, 50, a);

	change_source got = regions.get_source(100, 50);
	assert(same_source(got, a));
	got = regions.get_source(120, 10);
	assert(same_source(got, a));
}

static void test_overwrite_subrange()
{
	host_write_regions regions;
	change_source a = make_source(1);
	change_source b = make_source(2);

	regions.register_source(0, 100, a);
	regions.register_source(40, 20, b);

	change_source got = regions.get_source(40, 20);
	assert(same_source(got, b));
	got = regions.get_source(0, 40);
	assert(same_source(got, a));
	got = regions.get_source(60, 40);
	assert(same_source(got, a));
}

static void test_merge_adjacent()
{
	host_write_regions regions;
	change_source a = make_source(3);

	regions.register_source(0, 50, a);
	regions.register_source(50, 50, a);

	change_source got = regions.get_source(0, 100);
	assert(same_source(got, a));
}

static void test_stride()
{
	host_write_regions regions;
	change_source a = make_source(4);

	regions.register_source(0, 4, a, 3, 8);
	assert(same_source(regions.get_source(0, 4), a));
	assert(same_source(regions.get_source(8, 4), a));
	assert(same_source(regions.get_source(16, 4), a));
}

static void test_tightly_packed()
{
	host_write_regions regions;
	change_source a = make_source(5);

	regions.register_source(200, 4, a, 5, 0);
	change_source got = regions.get_source(200, 20);
	assert(same_source(got, a));
}

static void test_stats()
{
	host_write_regions regions;
	host_write_regions::stats stats = regions.get_stats();
	assert(stats.segments == 0);
	assert(stats.bytes == 0);

	change_source a = make_source(6);
	change_source b = make_source(7);
	change_source c = make_source(8);

	regions.register_source(0, 10, a);
	regions.register_source(20, 5, b);
	stats = regions.get_stats();
	assert(stats.segments == 2);
	assert(stats.bytes == 15);

	regions.register_source(10, 10, a); // merge with first
	stats = regions.get_stats();
	assert(stats.segments == 2);
	assert(stats.bytes == 25);

	regions.register_source(5, 25, c); // overwrite all and merge into one
	stats = regions.get_stats();
	assert(stats.segments == 2);
	assert(stats.bytes == 30);

	host_write_regions dst;
	dst.register_source(0, 5, a);
	dst.copy_sources(regions, 100, 10, 10);
	stats = dst.get_stats();
	assert(stats.segments == 2);
	assert(stats.bytes == 15);
}

static void test_copy_sources_basic()
{
	host_write_regions src;
	host_write_regions dst;
	change_source a = make_source(8);
	change_source b = make_source(9);

	src.register_source(0, 50, a);
	src.register_source(50, 50, b);

	dst.copy_sources(src, 100, 0, 100);
	assert(same_source(dst.get_source(100, 50), a));
	assert(same_source(dst.get_source(150, 50), b));
}

static void test_copy_sources_overlap()
{
	host_write_regions src;
	host_write_regions dst;
	change_source a = make_source(10);
	change_source b = make_source(11);
	change_source c = make_source(12);

	src.register_source(0, 100, a);
	dst.register_source(200, 100, b);

	dst.copy_sources(src, 250, 25, 50);
	assert(same_source(dst.get_source(250, 50), a));
	assert(same_source(dst.get_source(200, 50), b));

	dst.register_source(260, 10, c);
	dst.copy_sources(src, 240, 40, 20);
	assert(same_source(dst.get_source(240, 20), a));
	assert(same_source(dst.get_source(260, 10), c));
}

static void test_stats_dump_path()
{
	std::vector<trackedbuffer> buffers;
	std::vector<trackedimage> images;
	std::vector<trackedtensor> tensors;
	std::vector<trackedaccelerationstructure> accel;

	buffers.resize(2);
	buffers[0].index = 0;
	buffers[1].index = 1;

	images.resize(1);
	images[0].index = 2;

	tensors.resize(1);
	tensors[0].index = 3;

	accel.resize(1);
	accel[0].index = 4;

	change_source a = make_source(9);
	change_source b = make_source(10);
	change_source c = make_source(11);

	buffers[1].source.register_source(0, 8, a);
	buffers[1].source.register_source(16, 8, b);
	images[0].source.register_source(0, 4, c);

	host_write_totals buf_stats = gather_host_write_stats(buffers);
	host_write_totals img_stats = gather_host_write_stats(images);
	host_write_totals tensor_stats = gather_host_write_stats(tensors);
	host_write_totals accel_stats = gather_host_write_stats(accel);

	assert(buf_stats.objects == 2);
	assert(buf_stats.objects_with_data == 1);
	assert(buf_stats.segments == 2);
	assert(buf_stats.bytes == 16);
	assert(buf_stats.max_segments == 2);
	assert(buf_stats.max_index == 1);

	assert(img_stats.objects == 1);
	assert(img_stats.objects_with_data == 1);
	assert(img_stats.segments == 1);
	assert(img_stats.bytes == 4);
	assert(img_stats.max_segments == 1);
	assert(img_stats.max_index == 2);

	assert(tensor_stats.objects == 1);
	assert(tensor_stats.objects_with_data == 0);
	assert(tensor_stats.segments == 0);
	assert(tensor_stats.bytes == 0);

	assert(accel_stats.objects == 1);
	assert(accel_stats.objects_with_data == 0);
	assert(accel_stats.segments == 0);
	assert(accel_stats.bytes == 0);

	host_write_totals total;
	total.objects = buf_stats.objects + img_stats.objects + tensor_stats.objects + accel_stats.objects;
	total.objects_with_data = buf_stats.objects_with_data + img_stats.objects_with_data +
		tensor_stats.objects_with_data + accel_stats.objects_with_data;
	total.segments = buf_stats.segments + img_stats.segments + tensor_stats.segments + accel_stats.segments;
	total.bytes = buf_stats.bytes + img_stats.bytes + tensor_stats.bytes + accel_stats.bytes;

	assert(total.objects == 5);
	assert(total.objects_with_data == 2);
	assert(total.segments == 3);
	assert(total.bytes == 20);
}

static void test_concurrent_writes()
{
	host_write_regions regions;
	change_source a = make_source(6);
	change_source b = make_source(7);

	auto writer_a = [&regions, a]()
	{
		for (uint32_t i = 0; i < 1000; ++i)
		{
			regions.register_source((uint64_t)i * 64, 32, a);
		}
	};

	auto writer_b = [&regions, b]()
	{
		for (uint32_t i = 0; i < 1000; ++i)
		{
			regions.register_source(100000 + (uint64_t)i * 64, 32, b);
		}
	};

	std::thread t1(writer_a);
	std::thread t2(writer_b);
	t1.join();
	t2.join();

	assert(same_source(regions.get_source(0, 32), a));
	assert(same_source(regions.get_source(64, 32), a));
	assert(same_source(regions.get_source(100000, 32), b));
	assert(same_source(regions.get_source(100064, 32), b));
}

int main()
{
	test_contiguous_basic();
	test_overwrite_subrange();
	test_merge_adjacent();
	test_stride();
	test_tightly_packed();
	test_stats();
	test_copy_sources_basic();
	test_copy_sources_overlap();
	test_stats_dump_path();
	test_concurrent_writes();
	return 0;
}
