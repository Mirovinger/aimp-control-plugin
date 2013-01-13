// Copyright (c) 2013, Alexey Ivanov

#ifndef STRING_ENCODING_UTILS_H
#define STRING_ENCODING_UTILS_H

#include <string>

//! contains string encoding utilities.
namespace StringEncoding {

/*!
    \brief Converts string in Windows ANSI system encoding to UTF-16 encoding.
    \param string in Windows ANSI system encoding.
    \throw EncodingError if convertion fails.
*/
std::wstring system_ansi_encoding_to_utf16(const std::string& ansi_string); // throws EncodingError

/*!
    \brief Converts string in Windows ANSI system encoding to UTF-16 encoding. This function does not throw exception on errors.
    \param string in Windows ANSI system encoding.
    \return string in UTF-16 on success or empty string on error.
*/
std::wstring system_ansi_encoding_to_utf16_safe(const std::string& ansi_string); // throws ()

/*!
    \brief Converts string in UTF-16 encoding to Windows ANSI system encoding.
    \param string in UTF-16
    \throw EncodingError if convertion fails.
    \return string in Windows ANSI system encoding.
*/
std::string utf16_to_system_ansi_encoding(const std::wstring& utf16_string); // throws EncodingError

/*!
    \brief Converts string in UTF-16 encoding to Windows ANSI system encoding. This function does not throw exception on errors.
    \param string in string in UTF-16.
    \return string in Windows ANSI system encoding on success or empty string on error.
*/
std::string utf16_to_system_ansi_encoding_safe(const std::wstring& utf16_string); // throws ()

/*!
    \brief Converts buffer defined by begin-end pointers in UTF-16 encoding to Windows ANSI system encoding.
    \param begin pointer to buffer begin.
    \param end pointer to buffer end.
    \throw InvalidArgument if begin follows end.
    \throw EncodingError if convertion fails.
    \return string in Windows ANSI system encoding.
*/
std::string utf16_to_system_ansi_encoding(const WCHAR* begin, const WCHAR* end); // throws InvalidArgument, EncodingError

/*!
    \brief Converts string in UTF-16 encoding to UTF-8 encoding.
    \param string in UTF-16.
    \throw EncodingError if convertion fails.
    \return string in UTF-8.
*/
std::string utf16_to_utf8(const std::wstring& utf16_string); // throws EncodingError

/*!
    \brief Converts string in UTF-8 encoding to UTF-16 encoding.
    \param string in UTF-8.
    \throw EncodingError if convertion fails.
    \return string in UTF-16.
*/
std::wstring utf8_to_utf16(const std::string& utf8_string); // throws EncodingError

/*!
    \brief Base class for exceptions generated by StringEncoding functions.
    Used to be able to catch any StringEncoding exception like this:
        try { ..error code.. } catch (EncodingException&) { ..handle.. }
*/
class EncodingException : public std::exception
{
public:
    explicit EncodingException(const std::string& what_arg)
        :
        std::exception( what_arg.c_str() )
    {}
};

/*!
    \brief Error encoding exception.
           This exceprion is thrown by all convert functions if input string can not be converted to destination encoding.
*/
class EncodingError : public EncodingException
{
public:
    explicit EncodingError(const std::string& what_arg)
        :
        EncodingException(what_arg)
    {}
};

/*!
    \brief Invalid argument exception.
           This exceprion is thrown by some convert functions if input parameters are wrong.
*/
class InvalidArgument : public EncodingException
{
public:
    explicit InvalidArgument(const std::string& what_arg)
        :
        EncodingException(what_arg)
    {}
};

} // namespace StringEncoding

#endif // #ifndef STRING_ENCODING_UTILS_H
