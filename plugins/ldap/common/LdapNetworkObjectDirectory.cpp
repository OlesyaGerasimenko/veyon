/*
 * LdapNetworkObjectDirectory.cpp - provides a NetworkObjectDirectory for LDAP
 *
 * Copyright (c) 2017-2021 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Veyon - https://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "LdapConfiguration.h"
#include "LdapDirectory.h"
#include "LdapNetworkObjectDirectory.h"


LdapNetworkObjectDirectory::LdapNetworkObjectDirectory( const LdapConfiguration& ldapConfiguration,
														QObject* parent ) :
	NetworkObjectDirectory( ldapConfiguration.directoryName(), parent ),
	m_ldapDirectory( ldapConfiguration )
{
}



NetworkObjectList LdapNetworkObjectDirectory::queryObjects( NetworkObject::Type type,
															NetworkObject::Property property, const QVariant& value )
{
	switch( type )
	{
	case NetworkObject::Type::Location: return queryLocations( property, value );
	case NetworkObject::Type::Host: return queryHosts( property, value );
	default: break;
	}

	return {};
}



NetworkObjectList LdapNetworkObjectDirectory::queryParents( const NetworkObject& object )
{
	switch( object.type() )
	{
	case NetworkObject::Type::Host:
		return { NetworkObject( this, NetworkObject::Type::Location,
								m_ldapDirectory.locationsOfComputer( object.property( NetworkObject::Property::DirectoryAddress ).toString() ).value( 0 ) ) };
	case NetworkObject::Type::Location:
		return { rootObject() };
	default:
		break;
	}

	return { NetworkObject( this, NetworkObject::Type::None ) };
}



void LdapNetworkObjectDirectory::update()
{
	const auto locations = m_ldapDirectory.computerLocations();

	for( const auto& location : qAsConst( locations ) )
	{
		const NetworkObject locationObject{this, NetworkObject::Type::Location, location};

		addOrUpdateObject( locationObject, rootObject() );

		updateLocation( locationObject );
	}

	removeObjects( rootObject(), [locations]( const NetworkObject& object ) {
		return object.type() == NetworkObject::Type::Location && locations.contains( object.name() ) == false; } );
}



void LdapNetworkObjectDirectory::updateLocation( const NetworkObject& locationObject )
{
	const auto computers = m_ldapDirectory.computerLocationEntries( locationObject.name() );

	for( const auto& computer : qAsConst( computers ) )
	{
		const auto hostObject = computerToObject( this, &m_ldapDirectory, computer );
		if( hostObject.type() == NetworkObject::Type::Host )
		{
			addOrUpdateObject( hostObject, locationObject );
		}
	}

	removeObjects( locationObject, [computers]( const NetworkObject& object ) {
		return object.type() == NetworkObject::Type::Host &&
			   computers.contains( object.property( NetworkObject::Property::DirectoryAddress ).toString() ) == false; } );
}



NetworkObjectList LdapNetworkObjectDirectory::queryLocations( NetworkObject::Property property, const QVariant& value )
{
	QString name;

	switch( property )
	{
	case NetworkObject::Property::None:
		break;

	case NetworkObject::Property::Name:
		name = value.toString();
		break;

	default:
		vCritical() << "Can't query locations by property" << property;
		return {};
	}

	const auto locations = m_ldapDirectory.computerLocations( name );

	NetworkObjectList locationObjects;
	locationObjects.reserve( locations.size() );

	for( const auto& location : locations )
	{
		locationObjects.append( NetworkObject{this, NetworkObject::Type::Location, location} );
	}

	return locationObjects;
}



NetworkObjectList LdapNetworkObjectDirectory::queryHosts( NetworkObject::Property property, const QVariant& value )
{
	QStringList computers;

	switch( property )
	{
	case NetworkObject::Property::None:
		computers = m_ldapDirectory.computersByHostName( {} );
		break;

	case NetworkObject::Property::Name:
		computers = m_ldapDirectory.computersByDisplayName( value.toString() );
		break;

	case NetworkObject::Property::HostAddress:
	{
		const auto hostName = m_ldapDirectory.hostToLdapFormat( value.toString() );
		if( hostName.isEmpty() )
		{
			return {};
		}
		computers = m_ldapDirectory.computersByHostName( hostName );
		break;
	}

	default:
		vCritical() << "Can't query hosts by attribute" << property;
		return {};
	}

	NetworkObjectList hostObjects;
	hostObjects.reserve( computers.size() );

	for( const auto& computer : qAsConst(computers) )
	{
		const auto hostObject = computerToObject( this, &m_ldapDirectory, computer );
		if( hostObject.isValid() )
		{
			hostObjects.append( hostObject );
		}
	}

	return hostObjects;
}



NetworkObject LdapNetworkObjectDirectory::computerToObject( NetworkObjectDirectory* directory,
												  LdapDirectory* ldapDirectory, const QString& computerDn )
{
	auto displayNameAttribute = ldapDirectory->computerDisplayNameAttribute();
	if( displayNameAttribute.isEmpty() )
	{
		displayNameAttribute = LdapClient::cn();
	}

	auto hostNameAttribute = ldapDirectory->computerHostNameAttribute();
	if( hostNameAttribute.isEmpty() )
	{
		hostNameAttribute = LdapClient::cn();
	}

	QStringList computerAttributes{ LdapClient::cn(), displayNameAttribute, hostNameAttribute };

	auto macAddressAttribute = ldapDirectory->computerMacAddressAttribute();
	if( macAddressAttribute.isEmpty() == false )
	{
		computerAttributes.append( macAddressAttribute );
	}

	computerAttributes.removeDuplicates();

	const auto computers = ldapDirectory->client().queryObjects( computerDn, computerAttributes,
															 ldapDirectory->computersFilter(), LdapClient::Scope::Base );
	if( computers.isEmpty() == false )
	{
		const auto& computerDn = computers.firstKey();
		const auto& computer = computers.first();

		auto displayName = computer[displayNameAttribute].value( 0 );
		auto hostName = computer[hostNameAttribute].value( 0 );

		if( displayName.isEmpty() )
		{
			displayName = computer[LdapClient::cn()].value( 0 );
		}
		if( hostName.isEmpty() )
		{
			hostName = computer[LdapClient::cn()].value( 0 );
		}

		NetworkObject::Properties properties;
		properties[NetworkObject::propertyKey(NetworkObject::Property::HostAddress)] = hostName;
		if( macAddressAttribute.isEmpty() == false )
		{
			properties[NetworkObject::propertyKey(NetworkObject::Property::MacAddress)] =
				computer[macAddressAttribute].value( 0 );
		}
		properties[NetworkObject::propertyKey(NetworkObject::Property::DirectoryAddress)] = computerDn;

		return NetworkObject{directory, NetworkObject::Type::Host, displayName, properties};
	}

	return NetworkObject{directory, NetworkObject::Type::None};
}
