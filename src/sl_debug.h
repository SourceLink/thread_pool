#ifndef SO_DEBUG_H_
#define SO_DEBUG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_ENABLE_DEBUG


enum debug_level {    
	DEBUG_LEVEL_DISABLE = 0,    
	DEBUG_LEVEL_ERR, 
	DEBUG_LEVEL_INFO, 
	DEBUG_LEVEL_DEBUG
};

#ifdef CONFIG_ENABLE_DEBUG

#include <stdio.h>
#define  PRINT               printf

#define DEBUG_SET_LEVEL(x)  static int debug = x


#define ASSERT()											\
	do {													\
		PRINT("ASSERT: %s %s %d\n",							\
		__FILE__, __FUNCTION__, __LINE__);           		\
		while (1);                                         	\
	} while (0)

#define ERR(format, ...)                                	\
	do {                                                    \
		if (debug >= DEBUG_LEVEL_ERR) {                 	\
			PRINT("ERROR: " format "\n", ##__VA_ARGS__);        	\
		}                                               	\
	} while (0)

#define INFO(format, ...)                              		\
	do {                                                    \
		if (debug >= DEBUG_LEVEL_INFO) {                	\
			PRINT("INFO: " format "\n", ##__VA_ARGS__);         	\
		}                                               	\
	} while (0)

#define DEBUG(format, ...)                              	\
	do {                                                    \
		if (debug >= DEBUG_LEVEL_DEBUG) {               	\
			PRINT("DEBUG: " format "\n", ##__VA_ARGS__);         \
		}                                               	\
	} while (0)

#else

#define DEBUG_SET_LEVEL(x) 
#define ASSERT()
#define ERR(...)
#define INFO(...)
#define DEBUG(...)

#endif	/* CONFIG_ENABLE_DEBUG  */
#ifdef __cplusplus
}
#endif
/* 需要建立一个开关控制每个.c的调试 */

#endif /* SO_DEBUG_H_ */
