#ifndef H_SYSTEM_PYTHON
#define	H_SYSTEM_PYTHON

#if defined(__APPLE__)
#include <sys/types.h>
#endif

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

/* For Python 3, use the PyLong type throughout in place of PyInt */
#if PY_MAJOR_VERSION >= 3
#define PyInt_Check PyLong_Check
#define PyInt_AsLong PyLong_AsLong
#define PyInt_FromLong PyLong_FromLong
#define PyInt_AsUnsignedLongMask PyLong_AsUnsignedLongMask
#define PyInt_AsUnsignedLongLongMask PyLong_AsUnsignedLongLongMask
#define PyInt_AsSsize_t PyLong_AsSsize_t
#endif

PyObject * fakedecode;

static inline PyObject * utf8FromString(const char *s)
{
/* In Python 3, we return all strings as surrogate-escaped utf-8 */
#if PY_MAJOR_VERSION >= 3
    if (s != NULL) {
	PyObject *o = PyUnicode_DecodeUTF8(s, strlen(s), "surrogateescape");
	/* fish the fake decode function from python side if not done yet */
	if (fakedecode == NULL) {
	    PyObject *n = PyUnicode_FromString("rpm");
	    PyObject *m = PyImport_Import(n);
	    PyObject *md = PyModule_GetDict(m);
	    fakedecode = PyDict_GetItemString(md, "_fakedecode");
	    Py_DECREF(m);
	    Py_DECREF(n);
	}
	if (fakedecode && o) {
	    /* monkey-patch it into the string object as "decode" */
	    PyDict_SetItemString(Py_TYPE(o)->tp_dict, "decode", fakedecode);
	}
	return o;
    }
#else
    if (s != NULL)
	return PyBytes_FromString(s);
#endif
    Py_RETURN_NONE;
}

#endif	/* H_SYSTEM_PYTHON */
