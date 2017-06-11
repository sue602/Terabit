#ifndef __SHARE_HEADER__
#define __SHARE_HEADER__

#include "ace/ace.h"
#include "ace/OS.h"

#define ACE_static_cast(foo,bar) (static_cast<foo>(bar))
#define ACE_LIB_TEXT ACE_TEXT

#if !defined (ACE_ERROR_RETURN)
# define ACE_ERROR_RETURN(X, Y) return (Y)
#endif

#endif