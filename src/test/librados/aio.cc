#include <errno.h>
#include <fcntl.h>
#include <string>
#include <sstream>
#include <utility>
#include <boost/scoped_ptr.hpp>
#include <fmt/format.h>

#include "include/buffer.h"
#include "include/byteorder.h" // for ceph_le32
#include "include/err.h"
#include "include/rados/librados.h"
#include "include/types.h"
#include "include/stringify.h"
#include "include/scope_guard.h"

#include "common/errno.h"

#include "gtest/gtest.h"

#include "test.h"
#include "crimson_utils.h"

using std::ostringstream;

class AioTestData
{
public:
  AioTestData()
    : m_cluster(NULL),
      m_ioctx(NULL),
      m_init(false)
  {
  }

  ~AioTestData()
  {
    if (m_init) {
      rados_ioctx_destroy(m_ioctx);
      destroy_one_pool(m_pool_name, &m_cluster);
    }
  }

  std::string init(bool split_ops)
  {
    int ret;
    auto pool_prefix = fmt::format("{}_", ::testing::UnitTest::GetInstance()->current_test_info()->name());
    m_pool_name = get_temp_pool_name(pool_prefix);
    std::string err = create_one_pool(m_pool_name, &m_cluster);
    if (!err.empty()) {
      ostringstream oss;
      oss << "create_one_pool(" << m_pool_name << ") failed: error " << err;
      return oss.str();
    }
    // err = set_split_ops(m_pool_name, &m_cluster, split_ops);
    // if (!err.empty()) {
    //   ostringstream oss;
    //   oss << "create_one_ec_pool(" << m_pool_name << ") failed: error " << err;
    //   return oss.str();
    // }
    ret = rados_ioctx_create(m_cluster, m_pool_name.c_str(), &m_ioctx);
    if (ret) {
      destroy_one_pool(m_pool_name, &m_cluster);
      ostringstream oss;
      oss << "rados_ioctx_create failed: error " << ret;
      return oss.str();
    }
    m_init = true;
    return "";
  }

  rados_t m_cluster;
  rados_ioctx_t m_ioctx;
  std::string m_pool_name;
  bool m_init;
};

class LibRadosAio : public ::testing::TestWithParam<bool> {};

TEST_P(LibRadosAio, TooBig) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(-E2BIG, rados_aio_write(test_data.m_ioctx, "foo",
                                    my_completion, buf, UINT_MAX, 0));
  ASSERT_EQ(-E2BIG, rados_aio_write_full(test_data.m_ioctx, "foo",
                                         my_completion, buf, UINT_MAX));
  ASSERT_EQ(-E2BIG, rados_aio_append(test_data.m_ioctx, "foo",
                                     my_completion, buf, UINT_MAX));
  rados_aio_release(my_completion);
}

TEST_P(LibRadosAio, SimpleWrite) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  auto sg = make_scope_guard([&] { rados_aio_release(my_completion); });
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));

  rados_ioctx_set_namespace(test_data.m_ioctx, "nspace");
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  auto sg2 = make_scope_guard([&] { rados_aio_release(my_completion2); });
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion2, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
}

TEST_P(LibRadosAio, WaitForSafe) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  TestAlarm alarm;
  ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
}

TEST_P(LibRadosAio, RoundTrip) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[256];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAio, RoundTrip2) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAio, RoundTrip3) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));

  rados_write_op_t op1 = rados_create_write_op();
  rados_write_op_write(op1, buf, sizeof(buf), 0);
  rados_write_op_set_alloc_hint2(op1, 0, 0, LIBRADOS_OP_FLAG_FADVISE_DONTNEED); 
  ASSERT_EQ(0, rados_aio_write_op_operate(op1, test_data.m_ioctx, my_completion,
					  "foo", NULL, 0));
  rados_release_write_op(op1);

  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }

  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);

  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));

  rados_read_op_t op2 = rados_create_read_op();
  rados_read_op_read(op2, 0, sizeof(buf2), buf2, NULL, NULL);
  rados_read_op_set_flags(op2, LIBRADOS_OP_FLAG_FADVISE_NOCACHE |
			       LIBRADOS_OP_FLAG_FADVISE_RANDOM);
  ceph_le32 init_value(-1);
  ceph_le32 checksum[2];
  rados_read_op_checksum(op2, LIBRADOS_CHECKSUM_TYPE_CRC32C,
			 reinterpret_cast<char *>(&init_value),
			 sizeof(init_value), 0, 0, 0,
                         reinterpret_cast<char *>(&checksum),
                         sizeof(checksum), NULL);
  ASSERT_EQ(0, rados_aio_read_op_operate(op2, test_data.m_ioctx, my_completion2,
					 "foo", 0));
  rados_release_read_op(op2);
  
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion2);

  bufferlist bl;
  bl.append(buf, sizeof(buf));
  ASSERT_EQ(1U, checksum[0]);
  ASSERT_EQ(bl.crc32c(-1), checksum[1]);
}

TEST_P(LibRadosAio, RoundTripAppend) {
  AioTestData test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf2)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  char buf3[sizeof(buf) + sizeof(buf2)];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)sizeof(buf3), rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(0, memcmp(buf3, buf, sizeof(buf)));
  ASSERT_EQ(0, memcmp(buf3 + sizeof(buf), buf2, sizeof(buf2)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAio, RemoveTest) {
  char buf[128];
  char buf2[sizeof(buf)];
  rados_completion_t my_completion;
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  memset(buf, 0xaa, sizeof(buf));
  ASSERT_EQ(0, rados_append(test_data.m_ioctx, "foo", buf, sizeof(buf)));
  ASSERT_EQ(0, rados_aio_remove(test_data.m_ioctx, "foo", my_completion));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  memset(buf2, 0, sizeof(buf2));
  ASSERT_EQ(-ENOENT, rados_read(test_data.m_ioctx, "foo", buf2, sizeof(buf2), 0));
  rados_aio_release(my_completion);
}

TEST_P(LibRadosAio, XattrsRoundTrip) {
  char buf[128];
  char attr1[] = "attr1";
  char attr1_buf[] = "foo bar baz";
  // append
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));
  memset(buf, 0xaa, sizeof(buf));
  ASSERT_EQ(0, rados_append(test_data.m_ioctx, "foo", buf, sizeof(buf)));
  // async getxattr
  rados_completion_t my_completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  ASSERT_EQ(0, rados_aio_getxattr(test_data.m_ioctx, "foo", my_completion, attr1, buf, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(-ENODATA, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
  // async setxattr
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_setxattr(test_data.m_ioctx, "foo", my_completion2, attr1, attr1_buf, sizeof(attr1_buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  rados_aio_release(my_completion2);
  // async getxattr
  rados_completion_t my_completion3;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_getxattr(test_data.m_ioctx, "foo", my_completion3, attr1, buf, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)sizeof(attr1_buf), rados_aio_get_return_value(my_completion3));
  rados_aio_release(my_completion3);
  // check content of attribute
  ASSERT_EQ(0, memcmp(attr1_buf, buf, sizeof(attr1_buf)));
}

TEST_P(LibRadosAio, RmXattr) {
  char buf[128];
  char attr1[] = "attr1";
  char attr1_buf[] = "foo bar baz";
  // append
  memset(buf, 0xaa, sizeof(buf));
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_append(test_data.m_ioctx, "foo", buf, sizeof(buf)));  
  // async setxattr
  rados_completion_t my_completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  ASSERT_EQ(0, rados_aio_setxattr(test_data.m_ioctx, "foo", my_completion, attr1, attr1_buf, sizeof(attr1_buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
  // async rmxattr
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
            nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_rmxattr(test_data.m_ioctx, "foo", my_completion2, attr1));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  rados_aio_release(my_completion2);
  // async getxattr after deletion
  rados_completion_t my_completion3;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
            nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_getxattr(test_data.m_ioctx, "foo", my_completion3, attr1, buf, sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(-ENODATA, rados_aio_get_return_value(my_completion3));
  rados_aio_release(my_completion3);
  // Test rmxattr on a removed object
  char buf2[128];
  char attr2[] = "attr2";
  char attr2_buf[] = "foo bar baz";
  memset(buf2, 0xbb, sizeof(buf2));
  ASSERT_EQ(0, rados_write(test_data.m_ioctx, "foo_rmxattr", buf2, sizeof(buf2), 0));
  // asynx setxattr
  rados_completion_t my_completion4;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
            nullptr, &my_completion4));
  ASSERT_EQ(0, rados_aio_setxattr(test_data.m_ioctx, "foo_rmxattr", my_completion4, attr2, attr2_buf, sizeof(attr2_buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion4));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion4));
  rados_aio_release(my_completion4);
  // remove object
  ASSERT_EQ(0, rados_remove(test_data.m_ioctx, "foo_rmxattr"));
  // async rmxattr on non existing object
  rados_completion_t my_completion5;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
            nullptr, &my_completion5));
  ASSERT_EQ(0, rados_aio_rmxattr(test_data.m_ioctx, "foo_rmxattr", my_completion5, attr2));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion5));
  }
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(my_completion5));
  rados_aio_release(my_completion5);
}

TEST_P(LibRadosAio, XattrIter) {
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));
  // Create an object with 2 attributes
  char buf[128];
  char attr1[] = "attr1";
  char attr1_buf[] = "foo bar baz";
  char attr2[] = "attr2";
  char attr2_buf[256];
  for (size_t j = 0; j < sizeof(attr2_buf); ++j) {
    attr2_buf[j] = j % 0xff;
  }
  memset(buf, 0xaa, sizeof(buf));
  ASSERT_EQ(0, rados_append(test_data.m_ioctx, "foo", buf, sizeof(buf)));
  ASSERT_EQ(0, rados_setxattr(test_data.m_ioctx, "foo", attr1, attr1_buf, sizeof(attr1_buf)));
  ASSERT_EQ(0, rados_setxattr(test_data.m_ioctx, "foo", attr2, attr2_buf, sizeof(attr2_buf)));
  // call async version of getxattrs and wait for completion
  rados_completion_t my_completion;
  ASSERT_EQ(0, rados_aio_create_completion2((void*)&test_data,
            nullptr, &my_completion));
  rados_xattrs_iter_t iter;
  ASSERT_EQ(0, rados_aio_getxattrs(test_data.m_ioctx, "foo", my_completion, &iter));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  // loop over attributes
  int num_seen = 0;
  while (true) {
    const char *name;
    const char *val;
    size_t len;
    ASSERT_EQ(0, rados_getxattrs_next(iter, &name, &val, &len));
    if (name == NULL) {
      break;
    }
    ASSERT_LT(num_seen, 2);
    if ((strcmp(name, attr1) == 0) && (val != NULL) && (memcmp(val, attr1_buf, len) == 0)) {
      num_seen++;
      continue;
    }
    else if ((strcmp(name, attr2) == 0) && (val != NULL) && (memcmp(val, attr2_buf, len) == 0)) {
      num_seen++;
      continue;
    }
    else {
      ASSERT_EQ(0, 1);
    }
  }
  rados_getxattrs_end(iter);
}

TEST_P(LibRadosAio, IsComplete) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_complete.
    while (true) {
      int is_complete = rados_aio_is_complete(my_completion2);
      if (is_complete)
	break;
    }
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAio, IsSafe) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_safe.
    while (true) {
      int is_safe = rados_aio_is_safe(my_completion);
      if (is_safe)
	break;
    }
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAio, ReturnValue) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0, sizeof(buf));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "nonexistent",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
}

TEST_P(LibRadosAio, Flush) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xee, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  ASSERT_EQ(0, rados_aio_flush(test_data.m_ioctx));
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf2), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAio, FlushAsync) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  rados_completion_t flush_completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &flush_completion));
  char buf[128];
  memset(buf, 0xee, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  ASSERT_EQ(0, rados_aio_flush_async(test_data.m_ioctx, flush_completion));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(flush_completion));
  }
  ASSERT_EQ(1, rados_aio_is_complete(my_completion));
  ASSERT_EQ(1, rados_aio_is_complete(flush_completion));
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf2), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(flush_completion);
}

TEST_P(LibRadosAio, RoundTripWriteFull) {
  AioTestData test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[64];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_write_full(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf2)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  char buf3[sizeof(buf) + sizeof(buf2)];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)sizeof(buf2), rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(0, memcmp(buf3, buf2, sizeof(buf2)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAio, RoundTripWriteSame) {
  AioTestData test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char full[128];
  memset(full, 0xcc, sizeof(full));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, full, sizeof(full), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  /* write the same buf four times */
  char buf[32];
  size_t ws_write_len = sizeof(full);
  memset(buf, 0xdd, sizeof(buf));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_writesame(test_data.m_ioctx, "foo",
				   my_completion2, buf, sizeof(buf),
				   ws_write_len, 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, full, sizeof(full), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)sizeof(full), rados_aio_get_return_value(my_completion3));
  for (char *cmp = full; cmp < full + sizeof(full); cmp += sizeof(buf)) {
    ASSERT_EQ(0, memcmp(cmp, buf, sizeof(buf)));
  }
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAio, SimpleStat) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  uint64_t psize;
  time_t pmtime;
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion2, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(sizeof(buf), psize);
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAio, OperateMtime)
{
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));

  time_t set_mtime = 1457129052;
  {
    rados_write_op_t op = rados_create_write_op();
    rados_write_op_create(op, LIBRADOS_CREATE_IDEMPOTENT, nullptr);
    rados_completion_t completion;
    ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &completion));
    ASSERT_EQ(0, rados_aio_write_op_operate(op, test_data.m_ioctx, completion,
                                            "foo", &set_mtime, 0));
    {
      TestAlarm alarm;
      ASSERT_EQ(0, rados_aio_wait_for_complete(completion));
    }
    ASSERT_EQ(0, rados_aio_get_return_value(completion));
    rados_aio_release(completion);
    rados_release_write_op(op);
  }
  {
    uint64_t size;
    timespec mtime;
    ASSERT_EQ(0, rados_stat2(test_data.m_ioctx, "foo", &size, &mtime));
    EXPECT_EQ(0, size);
    EXPECT_EQ(set_mtime, mtime.tv_sec);
    EXPECT_EQ(0, mtime.tv_nsec);
  }
}

TEST_P(LibRadosAio, Operate2Mtime)
{
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));

  timespec set_mtime{1457129052, 123456789};
  {
    rados_write_op_t op = rados_create_write_op();
    rados_write_op_create(op, LIBRADOS_CREATE_IDEMPOTENT, nullptr);
    rados_completion_t completion;
    ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &completion));
    ASSERT_EQ(0, rados_aio_write_op_operate2(op, test_data.m_ioctx, completion,
                                             "foo", &set_mtime, 0));
    {
      TestAlarm alarm;
      ASSERT_EQ(0, rados_aio_wait_for_complete(completion));
    }
    ASSERT_EQ(0, rados_aio_get_return_value(completion));
    rados_aio_release(completion);
    rados_release_write_op(op);
  }
  {
    uint64_t size;
    timespec mtime;
    ASSERT_EQ(0, rados_stat2(test_data.m_ioctx, "foo", &size, &mtime));
    EXPECT_EQ(0, size);
    EXPECT_EQ(set_mtime.tv_sec, mtime.tv_sec);
    EXPECT_EQ(set_mtime.tv_nsec, mtime.tv_nsec);
  }
}

TEST_P(LibRadosAio, SimpleStatNS) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_ioctx_set_namespace(test_data.m_ioctx, "nspace");
  char buf2[64];
  memset(buf2, 0xbb, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  uint64_t psize;
  time_t pmtime;
  rados_completion_t my_completion2;
  rados_ioctx_set_namespace(test_data.m_ioctx, "");
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion2, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(sizeof(buf), psize);

  rados_ioctx_set_namespace(test_data.m_ioctx, "nspace");
  rados_completion_t my_completion3;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion3, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(sizeof(buf2), psize);

  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAio, StatRemove) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  uint64_t psize;
  time_t pmtime;
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion2, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(sizeof(buf), psize);
  rados_completion_t my_completion3;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_remove(test_data.m_ioctx, "foo", my_completion3));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion3));
  uint64_t psize2;
  time_t pmtime2;
  rados_completion_t my_completion4;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion4));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion4, &psize2, &pmtime2));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion4));
  }
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(my_completion4));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
  rados_aio_release(my_completion4);
}

TEST_P(LibRadosAio, ExecuteClass) {
  AioTestData test_data;
  rados_completion_t my_completion;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  }
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  char out[128];
  ASSERT_EQ(0, rados_aio_exec(test_data.m_ioctx, "foo", my_completion2,
			      "hello", "say_hello", NULL, 0, out, sizeof(out)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(13, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, strncmp("Hello, world!", out, 13));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

using std::string;
using std::map;
using std::set;

TEST_P(LibRadosAio, MultiWrite) {
  AioTestData test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));

  char buf2[64];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf2), sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));

  char buf3[(sizeof(buf) + sizeof(buf2)) * 3];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)(sizeof(buf) + sizeof(buf2)), rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(0, memcmp(buf3, buf, sizeof(buf)));
  ASSERT_EQ(0, memcmp(buf3 + sizeof(buf), buf2, sizeof(buf2)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAio, AioUnlock) {
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));
  ASSERT_EQ(0, rados_lock_exclusive(test_data.m_ioctx, "foo", "TestLock", "Cookie", "", NULL, 0));
  rados_completion_t my_completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
            nullptr, &my_completion));
  ASSERT_EQ(0, rados_aio_unlock(test_data.m_ioctx, "foo", "TestLock", "Cookie", my_completion));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  ASSERT_EQ(0, rados_lock_exclusive(test_data.m_ioctx, "foo", "TestLock", "Cookie", "", NULL,  0));
}

// EC test cases
class AioTestDataEC
{
public:
  AioTestDataEC()
    : m_cluster(NULL),
      m_ioctx(NULL),
      m_init(false)
  {
  }

  ~AioTestDataEC()
  {
    if (m_init) {
      rados_ioctx_destroy(m_ioctx);
      destroy_one_ec_pool(m_pool_name, &m_cluster);
    }
  }

  std::string init(bool fast_ec, bool split_ops)
  {
    int ret;
    auto pool_prefix = fmt::format("{}_", ::testing::UnitTest::GetInstance()->current_test_info()->name());
    m_pool_name = get_temp_pool_name(pool_prefix);
    std::string err = create_one_ec_pool(m_pool_name, &m_cluster, fast_ec);
    if (!err.empty()) {
      ostringstream oss;
      oss << "create_one_ec_pool(" << m_pool_name << ") failed: error " << err;
      return oss.str();
    }
    // err = set_split_ops(m_pool_name, &m_cluster, split_ops);
    // if (!err.empty()) {
    //   ostringstream oss;
    //   oss << "create_one_ec_pool(" << m_pool_name << ") failed: error " << err;
    //   return oss.str();
    // }
    ret = rados_ioctx_create(m_cluster, m_pool_name.c_str(), &m_ioctx);
    if (ret) {
      destroy_one_ec_pool(m_pool_name, &m_cluster);
      ostringstream oss;
      oss << "rados_ioctx_create failed: error " << ret;
      return oss.str();
    }
    m_init = true;
    return "";
  }

  rados_t m_cluster;
  rados_ioctx_t m_ioctx;
  std::string m_pool_name;
  bool m_init;
};

class LibRadosAioEC : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(LibRadosAioEC, SimpleWrite) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  auto sg = make_scope_guard([&] { rados_aio_release(my_completion); });
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));

  rados_ioctx_set_namespace(test_data.m_ioctx, "nspace");
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  auto sg2 = make_scope_guard([&] { rados_aio_release(my_completion2); });
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion2, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
}

TEST_P(LibRadosAioEC, WaitForComplete) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  TestAlarm alarm;
  ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
}

TEST_P(LibRadosAioEC, RoundTrip) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[256];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAioEC, RoundTrip2) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAioEC, RoundTripAppend) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion, my_completion2, my_completion3, my_completion4;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  int req;
  ASSERT_EQ(0, rados_ioctx_pool_requires_alignment2(test_data.m_ioctx, &req));
  ASSERT_NE(0, req);
  uint64_t alignment;
  ASSERT_EQ(0, rados_ioctx_pool_required_alignment2(test_data.m_ioctx, &alignment));
  ASSERT_NE(0U, alignment);

  int bsize = alignment;
  char *buf = (char *)new char[bsize];
  memset(buf, 0xcc, bsize);
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion, buf, bsize));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));

  int hbsize = bsize / 2;
  char *buf2 = (char *)new char[hbsize];
  memset(buf2, 0xdd, hbsize);
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion2, buf2, hbsize));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));

  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_append(test_data.m_ioctx, "foo",
			       my_completion3, buf2, hbsize));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  EXPECT_EQ(-EOPNOTSUPP, rados_aio_get_return_value(my_completion3));

  int tbsize = bsize + hbsize;
  char *buf3 = (char *)new char[tbsize];
  memset(buf3, 0, tbsize);
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion4));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion4, buf3, bsize * 3, 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion4));
  }
  ASSERT_EQ(tbsize, rados_aio_get_return_value(my_completion4));
  ASSERT_EQ(0, memcmp(buf3, buf, bsize));
  ASSERT_EQ(0, memcmp(buf3 + bsize, buf2, hbsize));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
  rados_aio_release(my_completion4);
  delete[] buf;
  delete[] buf2;
  delete[] buf3;
}

TEST_P(LibRadosAioEC, IsComplete) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_complete.
    while (true) {
      int is_complete = rados_aio_is_complete(my_completion2);
      if (is_complete)
	break;
    }
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAioEC, IsSafe) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;

    // Busy-wait until the AIO completes.
    // Normally we wouldn't do this, but we want to test rados_aio_is_safe.
    while (true) {
      int is_safe = rados_aio_is_safe(my_completion);
      if (is_safe)
	break;
    }
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAioEC, ReturnValue) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0, sizeof(buf));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "nonexistent",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(my_completion));
  rados_aio_release(my_completion);
}

TEST_P(LibRadosAioEC, Flush) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xee, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  ASSERT_EQ(0, rados_aio_flush(test_data.m_ioctx));
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf2), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAioEC, FlushAsync) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  rados_completion_t flush_completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &flush_completion));
  char buf[128];
  memset(buf, 0xee, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  ASSERT_EQ(0, rados_aio_flush_async(test_data.m_ioctx, flush_completion));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(flush_completion));
  }
  ASSERT_EQ(1, rados_aio_is_complete(my_completion));
  ASSERT_EQ(1, rados_aio_is_complete(flush_completion));
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[128];
  memset(buf2, 0, sizeof(buf2));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion2, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ((int)sizeof(buf2), rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(flush_completion);
}

TEST_P(LibRadosAioEC, RoundTripWriteFull) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  char buf2[64];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_write_full(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf2)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  char buf3[sizeof(buf) + sizeof(buf2)];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)sizeof(buf2), rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(0, memcmp(buf3, buf2, sizeof(buf2)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAioEC, SimpleStat) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  uint64_t psize;
  time_t pmtime;
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion2, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(sizeof(buf), psize);
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}


TEST_P(LibRadosAioEC, SimpleStatNS) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_ioctx_set_namespace(test_data.m_ioctx, "nspace");
  char buf2[64];
  memset(buf2, 0xbb, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf2, sizeof(buf2), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  uint64_t psize;
  time_t pmtime;
  rados_completion_t my_completion2;
  rados_ioctx_set_namespace(test_data.m_ioctx, "");
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion2, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(sizeof(buf), psize);

  rados_ioctx_set_namespace(test_data.m_ioctx, "nspace");
  rados_completion_t my_completion3;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion3, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(sizeof(buf2), psize);

  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAioEC, StatRemove) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  uint64_t psize;
  time_t pmtime;
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion2, &psize, &pmtime));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(sizeof(buf), psize);
  rados_completion_t my_completion3;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_remove(test_data.m_ioctx, "foo", my_completion3));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion3));
  uint64_t psize2;
  time_t pmtime2;
  rados_completion_t my_completion4;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion4));
  ASSERT_EQ(0, rados_aio_stat(test_data.m_ioctx, "foo",
			      my_completion4, &psize2, &pmtime2));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion4));
  }
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(my_completion4));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
  rados_aio_release(my_completion4);
}

TEST_P(LibRadosAioEC, ExecuteClass) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));
  rados_completion_t my_completion2;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  char out[128];
  ASSERT_EQ(0, rados_aio_exec(test_data.m_ioctx, "foo", my_completion2,
			      "hello", "say_hello", NULL, 0, out, sizeof(out)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(13, rados_aio_get_return_value(my_completion2));
  ASSERT_EQ(0, strncmp("Hello, world!", out, 13));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
}

TEST_P(LibRadosAioEC, MultiWrite) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  rados_completion_t my_completion, my_completion2, my_completion3;
  const auto& params = GetParam();
  bool fast_ec = std::get<0>(params);
  bool split_ops = std::get<1>(params);
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion));
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion, buf, sizeof(buf), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(my_completion));

  char buf2[64];
  memset(buf2, 0xdd, sizeof(buf2));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion2));
  ASSERT_EQ(0, rados_aio_write(test_data.m_ioctx, "foo",
			       my_completion2, buf2, sizeof(buf2), sizeof(buf)));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion2));
  }
  ASSERT_EQ(-EOPNOTSUPP, rados_aio_get_return_value(my_completion2));

  char buf3[(sizeof(buf) + sizeof(buf2)) * 3];
  memset(buf3, 0, sizeof(buf3));
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr,
	      nullptr, &my_completion3));
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "foo",
			      my_completion3, buf3, sizeof(buf3), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(my_completion3));
  }
  ASSERT_EQ((int)sizeof(buf), rados_aio_get_return_value(my_completion3));
  ASSERT_EQ(0, memcmp(buf3, buf, sizeof(buf)));
  rados_aio_release(my_completion);
  rados_aio_release(my_completion2);
  rados_aio_release(my_completion3);
}

TEST_P(LibRadosAio, CancelBeforeSubmit) {
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));

  rados_completion_t completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &completion));

  ASSERT_EQ(0, rados_aio_cancel(test_data.m_ioctx, completion));
  rados_aio_release(completion);
}

TEST_P(LibRadosAio, CancelBeforeComplete) {
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));

  // cancellation tests are racy, so retry if completion beats the cancellation
  int ret = 0;
  int tries = 10;
  do {
    rados_completion_t completion;
    ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &completion));
    char buf[128];
    ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "nonexistent",
                                completion, buf, sizeof(buf), 0));

    ASSERT_EQ(0, rados_aio_cancel(test_data.m_ioctx, completion));
    {
      TestAlarm alarm;
      ASSERT_EQ(0, rados_aio_wait_for_complete(completion));
    }
    ret = rados_aio_get_return_value(completion);
    rados_aio_release(completion);
  } while (ret == -ENOENT && --tries);

  ASSERT_EQ(-ECANCELED, ret);
}

TEST_P(LibRadosAio, CancelAfterComplete) {
  AioTestData test_data;
  rados_completion_t completion;
  ASSERT_EQ("", test_data.init(GetParam()));

  ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &completion));
  char buf[128];
  ASSERT_EQ(0, rados_aio_read(test_data.m_ioctx, "nonexistent",
                              completion, buf, sizeof(buf), 0));

  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(completion));
  }
  ASSERT_EQ(0, rados_aio_cancel(test_data.m_ioctx, completion));
  ASSERT_EQ(-ENOENT, rados_aio_get_return_value(completion));
  rados_aio_release(completion);
}

// Context structure for read/write ordering test
struct ReadWriteOrderingContext {
  rados_ioctx_t ioctx;
  char* write_buf;
  char* read_buf;
  int current_value;
  int iteration;
  int max_iterations;
  rados_completion_t write_completion;
  rados_completion_t read_completion;
  bool test_failed;
  std::string error_msg;
};

// Callback for read completion - decode value, increment, and issue next write
static void read_complete_callback(rados_completion_t c, void *arg) {
  ReadWriteOrderingContext* ctx = static_cast<ReadWriteOrderingContext*>(arg);
  
  // Check if read succeeded
  int ret = rados_aio_get_return_value(c);
  if (ret < 0) {
    ctx->test_failed = true;
    ctx->error_msg = "Read failed with error: " + std::to_string(ret);
    return;
  }
  
  // Decode the integer from the read buffer
  int read_value;
  memcpy(&read_value, ctx->read_buf, sizeof(int));
  
  std::cout << "Iteration " << ctx->iteration << ": Read value = " << read_value
            << ", Expected = " << ctx->current_value << std::endl;
  
  // Verify the value matches expected
  if (read_value != ctx->current_value) {
    ctx->test_failed = true;
    ctx->error_msg = "Read value mismatch: expected " +
                     std::to_string(ctx->current_value) +
                     " but got " + std::to_string(read_value);
    return;
  }
  
  // Increment for next iteration
  ctx->current_value++;
  ctx->iteration++;
  
  // If we haven't reached max iterations, issue next write
  if (ctx->iteration < ctx->max_iterations) {
    memcpy(ctx->write_buf, &ctx->current_value, sizeof(int));
    
    std::cout << "Iteration " << ctx->iteration << ": Writing value = " << ctx->current_value << std::endl;
    
    // Create new write completion
    int r = rados_aio_create_completion2(ctx, nullptr, &ctx->write_completion);
    if (r < 0) {
      ctx->test_failed = true;
      ctx->error_msg = "Failed to create write completion: " + std::to_string(r);
      return;
    }
    
    // Issue async write
    r = rados_aio_write(ctx->ioctx, "ordering_test_obj",
                        ctx->write_completion, ctx->write_buf, sizeof(int), 0);
    if (r < 0) {
      ctx->test_failed = true;
      ctx->error_msg = "Write failed with error: " + std::to_string(r);
      rados_aio_release(ctx->write_completion);
      return;
    }
    
    // Immediately issue read with ORDER_READS_WRITES flag
    r = rados_aio_create_completion2(ctx, read_complete_callback, &ctx->read_completion);
    if (r < 0) {
      ctx->test_failed = true;
      ctx->error_msg = "Failed to create read completion: " + std::to_string(r);
      return;
    }
    
    rados_read_op_t read_op = rados_create_read_op();
    rados_read_op_read(read_op, 0, sizeof(int), ctx->read_buf, NULL, NULL);
    r = rados_aio_read_op_operate(read_op, ctx->ioctx, ctx->read_completion,
                                   "ordering_test_obj",
                                   LIBRADOS_OPERATION_ORDER_READS_WRITES);
    rados_release_read_op(read_op);
    if (r < 0) {
      ctx->test_failed = true;
      ctx->error_msg = "Read failed with error: " + std::to_string(r);
      rados_aio_release(ctx->read_completion);
      return;
    }
  }
}

// Shared implementation for read/write ordering test
static void test_read_write_ordering_impl(rados_ioctx_t ioctx) {
  const int max_iterations = 20;
  char write_buf[sizeof(int)];
  char read_buf[sizeof(int)];
  
  // Initialize context
  ReadWriteOrderingContext ctx;
  ctx.ioctx = ioctx;
  ctx.write_buf = write_buf;
  ctx.read_buf = read_buf;
  ctx.current_value = 1;
  ctx.iteration = 0;
  ctx.max_iterations = max_iterations;
  ctx.test_failed = false;
  
  // Create the object with initial value 0 (without ordering flag, wait for completion)
  memcpy(write_buf, &ctx.current_value, sizeof(int));
  rados_completion_t create_completion;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &create_completion));
  ASSERT_EQ(0, rados_aio_write(ioctx, "ordering_test_obj",
                               create_completion, write_buf, sizeof(int), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(create_completion));
  }
  ASSERT_EQ(0, rados_aio_get_return_value(create_completion));
  rados_aio_release(create_completion);
  
  // Start the read/write loop with ordering
  ctx.current_value = 1;  // Next value to write
  ctx.iteration = 1;      // We've done iteration 0 (initial write)
  
  // Write value 1
  memcpy(write_buf, &ctx.current_value, sizeof(int));
  std::cout << "Iteration " << ctx.iteration << ": Writing value = " << ctx.current_value << std::endl;
  ASSERT_EQ(0, rados_aio_create_completion2(&ctx, nullptr, &ctx.write_completion));
  ASSERT_EQ(0, rados_aio_write(ioctx, "ordering_test_obj",
                               ctx.write_completion, write_buf, sizeof(int), 0));
  
  // Immediately read with ORDER_READS_WRITES flag (should see value 1 due to ordering)
  ASSERT_EQ(0, rados_aio_create_completion2(&ctx, read_complete_callback,
                                            &ctx.read_completion));
  
  rados_read_op_t read_op = rados_create_read_op();
  rados_read_op_read(read_op, 0, sizeof(int), read_buf, NULL, NULL);
  ASSERT_EQ(0, rados_aio_read_op_operate(read_op, ioctx, ctx.read_completion,
                                         "ordering_test_obj",
                                         LIBRADOS_OPERATION_ORDER_READS_WRITES));
  rados_release_read_op(read_op);
  
  // Wait for all operations to complete
  // The callback chain will continue until max_iterations is reached
  {
    TestAlarm alarm;
    while (ctx.iteration < max_iterations && !ctx.test_failed) {
      usleep(10000);  // Sleep 10ms between checks
    }
  }
  
  // Check for any errors during the test
  ASSERT_FALSE(ctx.test_failed) << "Test failed: " << ctx.error_msg;
  
  // Wait for final operations to complete
  if (ctx.iteration >= max_iterations) {
    TestAlarm alarm;
    if (ctx.write_completion) {
      rados_aio_wait_for_complete(ctx.write_completion);
      rados_aio_release(ctx.write_completion);
    }
    if (ctx.read_completion) {
      rados_aio_wait_for_complete(ctx.read_completion);
      rados_aio_release(ctx.read_completion);
    }
  }
  
  // Verify final value is 20
  char final_buf[sizeof(int)];
  rados_completion_t final_read;
  ASSERT_EQ(0, rados_aio_create_completion2(nullptr, nullptr, &final_read));
  ASSERT_EQ(0, rados_aio_read(ioctx, "ordering_test_obj",
                              final_read, final_buf, sizeof(int), 0));
  {
    TestAlarm alarm;
    ASSERT_EQ(0, rados_aio_wait_for_complete(final_read));
  }
  ASSERT_EQ((int)sizeof(int), rados_aio_get_return_value(final_read));
  
  int final_value;
  memcpy(&final_value, final_buf, sizeof(int));
  ASSERT_EQ(max_iterations - 1, final_value) << "Final value should be " << max_iterations;
  
  rados_aio_release(final_read);
}

TEST_P(LibRadosAio, ReadWriteOrdering) {
  AioTestData test_data;
  ASSERT_EQ("", test_data.init(GetParam()));
  test_read_write_ordering_impl(test_data.m_ioctx);
}

TEST_P(LibRadosAioEC, ReadWriteOrdering) {
  SKIP_IF_CRIMSON();
  AioTestDataEC test_data;
  auto [fast_ec, split_ops] = GetParam();
  ASSERT_EQ("", test_data.init(fast_ec, split_ops));
  test_read_write_ordering_impl(test_data.m_ioctx);
}

INSTANTIATE_TEST_SUITE_P_REPLICA(LibRadosAio);
INSTANTIATE_TEST_SUITE_P_EC(LibRadosAioEC);
