/***************************************************************************
                          host.h  -  description
                             -------------------
    begin                : Mar nov 12 2002
    copyright            : (C) 2002 by spe
    email                : spe@artik.intra.selectbourse.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef HOST_H
#define HOST_H

#include "string.h"
#include "debugtk.h"

/**
  *@author spe
  */

class Host {
private:
  String *address;
  unsigned short port;
public: 
	Host(String *_address, unsigned short port);
	~Host();
  String *getAddress(void) { return address; };
  unsigned short getPort(void) { return port; };
};

#endif
