#include "stdafx.h"
#include "CppUnitTest.h"
#include "../picoquictest/picoquictest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace UnitTest1
{		
	TEST_CLASS(UnitTest1)
	{
	public:
		TEST_METHOD(test_picohash)
		{
            int ret = picohash_test();

            Assert::AreEqual(ret, 0); 
		}

        TEST_METHOD(test_cnxcreation)
        {
            int ret = cnxcreation_test();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_parse_header)
        {
            int ret = parseheadertest();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_pn2pn64)
        {
            int ret = pn2pn64test();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_intformat)
        {
            int ret = intformattest();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_fnv1a)
        {
            int ret = fnv1atest();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_sack)
        {
            int ret = sacktest();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_float16)
        {
            int ret = float16test();

            Assert::AreEqual(ret, 0);
        }

        TEST_METHOD(test_StreamZeroFrame)
        {
            int ret = StreamZeroFrameTest();

            Assert::AreEqual(ret, 0);
        }

		TEST_METHOD(test_sendsack)
		{
			int ret = sendacktest();

			Assert::AreEqual(ret, 0);
		}


		TEST_METHOD(test_tls_api)
		{
			int ret = tls_api_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(test_tls_api_first_loss)
		{
			int ret = tls_api_loss_test(1ull);

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(test_tls_api_second_loss)
		{
			int ret = tls_api_loss_test(2ull);

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(test_tls_api_client_losses)
		{
			int ret = tls_api_loss_test(3ull);

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(test_tls_api_server_losses)
		{
			int ret = tls_api_loss_test(6ull);

			Assert::AreEqual(ret, 0);
		}
	};
}