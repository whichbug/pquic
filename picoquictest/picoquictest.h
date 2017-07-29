#ifndef PICOQUICTEST_H
#define PICOQUICTEST_H

#ifdef  __cplusplus
extern "C" {
#endif

    int picohash_test();
    int cnxcreation_test();
    int parseheadertest();
    int pn2pn64test();
    int intformattest();
    int fnv1atest();
    int sacktest();
    int float16test();
    int StreamZeroFrameTest();
	int sendacktest();
    int tls_api_test(); 
	int tls_api_loss_test(uint64_t mask);
	int tls_api_many_losses();

#ifdef  __cplusplus
}
#endif

#endif /* PICOQUICTEST_H */
