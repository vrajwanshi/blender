/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief Class to encapsulate an array of RGB or Gray level values
 */

#include <string.h>  // for memcpy

#include "MEM_guardedalloc.h"

namespace Freestyle {

//
// Image base class, for all types of images
//
///////////////////////////////////////////////////////////////////////////////

/** This class allows the storing of part of an image, while allowing a normal access to its pixel
 * values. You can for example only a rectangle of sw*sh, whose lower-left corner is at (ox, oy),
 * of an image of size w*h, and access these pixels using x,y coordinates specified in the whole
 * image coordinate system.
 */
class FrsImage {
 public:
  /** Default constructor */
  FrsImage()
  {
    _storedWidth = 0;
    _storedHeight = 0;
    _width = 0;
    _height = 0;
    _Ox = 0;
    _Oy = 0;
  }

  /** Copy constructor */
  FrsImage(const FrsImage &brother)
  {
    _storedWidth = brother._storedWidth;
    _storedHeight = brother._storedHeight;
    _width = brother._width;
    _height = brother._height;
    _Ox = brother._Ox;
    _Oy = brother._Oy;
  }

  /** Builds an FrsImage from its width and height.
   *  The memory is allocated consequently.
   */
  FrsImage(uint w, uint h)
  {
    _width = w;
    _height = h;
    _storedWidth = w;
    _storedHeight = h;
    _Ox = 0;
    _Oy = 0;
  }

  /** Builds a partial-storing image.
   *  \param w:
   *    The width of the complete image
   *  \param h:
   *    The height of the complete image
   *  \param sw:
   *    The width of the rectangle that will actually be stored.
   *  \param sh:
   *    The height of the rectangle that will actually be stored.
   *  \param ox:
   *    The x-abscissa of the origin of the rectangle that will actually be stored.
   *  \param oy:
   *    The x-abscissa of the origin of the rectangle that will actually be stored.
   */
  FrsImage(uint w, uint h, uint sw, uint sh, uint ox, uint oy)
  {
    _width = w;
    _height = h;
    _storedWidth = sw;
    _storedHeight = sh;
    _Ox = ox;
    _Oy = oy;
  }

  /** Operator= */
  FrsImage &operator=(const FrsImage &brother)
  {
    _width = brother._width;
    _height = brother._height;
    _storedWidth = brother._storedWidth;
    _storedHeight = brother._storedHeight;
    _Ox = brother._Ox;
    _Oy = brother._Oy;
    return *this;
  }

  /** Destructor */
  virtual ~FrsImage() {}

  /** Returns the width of the complete image */
  inline uint width() const
  {
    return _width;
  }

  /** Returns the height of the complete image */
  inline uint height() const
  {
    return _height;
  }

  /** Returns the gray value for pixel x,y */
  virtual float pixel(uint x, uint y) const = 0;

  /** Sets the array.
   *  \param array:
   *    The array containing the values we wish to store.
   *    Its size is sw*sh.
   *  \param width:
   *    The width of the complete image
   *  \param height:
   *    The height of the complete image
   *  \param sw:
   *    The width of the rectangle that will actually be stored.
   *  \param sh:
   *    The height of the rectangle that will actually be stored.
   *  \param ox:
   *    The x-abscissa of the origin of the rectangle that will actually be stored.
   *  \param oy:
   *    The x-abscissa of the origin of the rectangle that will actually be stored.
   *  \param copy:
   *    If true, the array is copied, otherwise the pointer is copied
   */
  virtual void setArray(float *array,
                        uint width,
                        uint height,
                        uint sw,
                        uint sh,
                        uint x,
                        uint y,
                        bool copy = true) = 0;

  /** Returns the array containing the pixels values.
   *  Its size is sw*sh, i.e. potentially a smaller rectangular part of the complete image.
   */
  virtual float *getArray() = 0;

 protected:
  uint _width;
  uint _height;
  uint _storedWidth;
  uint _storedHeight;
  uint _Ox;  // origin of the stored part
  uint _Oy;  // origin of the stored part

  MEM_CXX_CLASS_ALLOC_FUNCS("Freestyle:FrsImage")
};

//
// RGBImage
//
///////////////////////////////////////////////////////////////////////////////
class RGBImage : public FrsImage {
 public:
  RGBImage() : FrsImage()
  {
    _rgb = 0;
  }

  RGBImage(const RGBImage &brother) : FrsImage(brother)
  {
    _rgb = new float[3 * _storedWidth * _storedHeight];
    memcpy(_rgb, brother._rgb, 3 * _storedWidth * _storedHeight * sizeof(float));
  }

  RGBImage(uint w, uint h) : FrsImage(w, h)
  {
    _rgb = new float[3 * _width * _height];
  }

  RGBImage(float *rgb, uint w, uint h) : FrsImage(w, h)
  {
    _rgb = new float[3 * _width * _height];
    memcpy(_rgb, rgb, 3 * _width * _height * sizeof(float));
  }

  /** Builds an RGB partial image from the useful part buffer.
   *  \param rgb:
   *    The array of size 3*sw*sh containing the RGB values of the sw*sh pixels we need to stored.
   *    These sw*sh pixels constitute a rectangular part of a bigger
   *    RGB image containing w*h pixels.
   *  \param w:
   *    The width of the complete image
   *  \param h:
   *    The height of the complete image
   *  \param sw:
   *    The width of the part of the image we want to store and work on
   *  \param sh:
   *    The height of the part of the image we want to store and work on
   */
  RGBImage(float *rgb, uint w, uint h, uint sw, uint sh, uint ox, uint oy)
      : FrsImage(w, h, sw, sh, ox, oy)
  {
    _rgb = new float[3 * _storedWidth * _storedHeight];
    memcpy(_rgb, rgb, 3 * _storedWidth * _storedHeight * sizeof(float));
  }

  RGBImage &operator=(const RGBImage &brother)
  {
    dynamic_cast<FrsImage &>(*this) = brother;
    _rgb = new float[3 * _storedWidth * _storedHeight];
    memcpy(_rgb, brother._rgb, 3 * _storedWidth * _storedHeight * sizeof(float));
    return *this;
  }

  virtual ~RGBImage()
  {
    if (_rgb) {
      delete[] _rgb;
    }
  }

  inline float getR(uint x, uint y) const
  {
    return _rgb[3 * (y - _Oy) * _storedWidth + (x - _Ox) * 3];
  }

  inline float getG(uint x, uint y) const
  {
    return _rgb[3 * (y - _Oy) * _storedWidth + (x - _Ox) * 3 + 1];
  }

  inline float getB(uint x, uint y) const
  {
    return _rgb[3 * (y - _Oy) * _storedWidth + (x - _Ox) * 3 + 2];
  }

  virtual void setPixel(uint x, uint y, float r, float g, float b)
  {
    float *tmp = &(_rgb[3 * (y - _Oy) * _storedWidth + (x - _Ox) * 3]);
    *tmp = r;
    tmp++;
    *tmp = g;
    tmp++;
    *tmp = b;
  }

  virtual float pixel(uint x, uint y) const
  {
    float res = 0.0f;
    float *tmp = &(_rgb[3 * (y - _Oy) * _storedWidth + (x - _Ox) * 3]);
    res += 11.0f * (*tmp);
    tmp++;
    res += 16.0f * (*tmp);
    tmp++;
    res += 5.0f * (*tmp);
    return res / 32.0f;
  }

  /** Sets the RGB array.
   *    copy
   *      If true, the array is copied, otherwise the pointer is copied
   */
  virtual void setArray(
      float *rgb, uint width, uint height, uint sw, uint sh, uint x, uint y, bool copy = true)
  {
    _width = width;
    _height = height;
    _storedWidth = sw;
    _storedHeight = sh;
    _Ox = x;
    _Oy = y;
    if (!copy) {
      _rgb = rgb;
      return;
    }

    memcpy(_rgb, rgb, 3 * _storedWidth * _storedHeight * sizeof(float));
  }

  virtual float *getArray()
  {
    return _rgb;
  }

 protected:
  float *_rgb;
};

//
// GrayImage
//
///////////////////////////////////////////////////////////////////////////////

class GrayImage : public FrsImage {
 public:
  GrayImage() : FrsImage()
  {
    _lvl = 0;
  }

  GrayImage(const GrayImage &brother) : FrsImage(brother)
  {
    _lvl = new float[_storedWidth * _storedHeight];
    memcpy(_lvl, brother._lvl, _storedWidth * _storedHeight * sizeof(*_lvl));
  }

  /** Builds an empty gray image */
  GrayImage(uint w, uint h) : FrsImage(w, h)
  {
    _lvl = new float[_width * _height];
  }

  GrayImage(float *lvl, uint w, uint h) : FrsImage(w, h)
  {
    _lvl = new float[_width * _height];
    memcpy(_lvl, lvl, _width * _height * sizeof(*_lvl));
  }

  /** Builds a partial image from the useful part buffer.
   *  \param lvl:
   *    The array of size sw*sh containing the gray values of the sw*sh pixels we need to stored.
   *    These sw*sh pixels constitute a rectangular part of a bigger
   *    gray image containing w*h pixels.
   *  \param w:
   *    The width of the complete image
   *  \param h:
   *    The height of the complete image
   *  \param sw:
   *    The width of the part of the image we want to store and work on
   *  \param sh:
   *    The height of the part of the image we want to store and work on
   */
  GrayImage(float *lvl, uint w, uint h, uint sw, uint sh, uint ox, uint oy)
      : FrsImage(w, h, sw, sh, ox, oy)
  {
    _lvl = new float[_storedWidth * _storedHeight];
    memcpy(_lvl, lvl, _storedWidth * _storedHeight * sizeof(float));
  }

  GrayImage &operator=(const GrayImage &brother)
  {
    dynamic_cast<FrsImage &>(*this) = brother;
    _lvl = new float[_storedWidth * _storedHeight];
    memcpy(_lvl, brother._lvl, _storedWidth * _storedHeight * sizeof(float));
    return *this;
  }

  virtual ~GrayImage()
  {
    if (_lvl) {
      delete[] _lvl;
    }
  }

  inline void setPixel(uint x, uint y, float v)
  {
    _lvl[(y - _Oy) * _storedWidth + (x - _Ox)] = v;
  }

  inline float pixel(uint x, uint y) const
  {
    return _lvl[(y - _Oy) * _storedWidth + (x - _Ox)];
  }

  /** Sets the array.
   *    copy
   *      If true, the array is copied, otherwise the pounsigneder is copied
   */
  void setArray(
      float *lvl, uint width, uint height, uint sw, uint sh, uint x, uint y, bool copy = true)
  {
    _width = width;
    _height = height;
    _storedWidth = sw;
    _storedHeight = sh;
    _Ox = x;
    _Oy = y;
    if (!copy) {
      _lvl = lvl;
      return;
    }

    memcpy(_lvl, lvl, _storedWidth * _storedHeight * sizeof(float));
  }

  /** Returns the array containing the gray values. */
  virtual float *getArray()
  {
    return _lvl;
  }

 protected:
  float *_lvl;
};

} /* namespace Freestyle */
