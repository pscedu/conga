# CONGA API #

Centralized Openflow and Network Governing Authority

Copyright © 2015, Pittsburgh Supercomputing Center.  All Rights Reserved.

---

## Table of Contents ##

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [Service Types](#service-types)
4. [Network Allocation](#network-allocation)
5. [Authentication](#authentication-1)

---

## Introduction ##

This API allows you to request priority network resources through CONGA using HTTP requests. Any tool that understands HTTP can communicate with the API by requesting the correct URI. Each URI represents a service offered by CONGA.

---

## Getting Started ##

The service is implemented using a REST architecture. Requests to the service are sent in a HTTP header over HTTPS. Data payloads shall be in JSON format.


#### HTTP Methods ####

This API uses appropriate HTTP verbs.

Method | Description
------ | -----------
GET    | Used for retrieving resources.
POST   | Used for creating resources and performing resource actions.
PUT    | Used for updating resources.
DELETE | Used for deleting resources.


#### Request ####

The base URI for requests is http://[CONGA-host]/api/ .
Each type of service has a unique URI in the format, http://[CONGA-host]/api/[service-name] . For example the URI for authentication actions is http://[CONGA-host]/api/auth . Every service type responses differently based on the HTTP verb that is used (see table above).


#### Response ####

Responses will be in JSON format.

Node Name | Description
--------- | -----------
status    | **integer**<br>  This is an integer code. It will be zero for a successful response and non-zero for an error. A description of the error, if available, will be in the errors node.
results   | **array**<br>   Holds either the requested data or a response from the successfully performed action.
errors    | **array**<br>   An error description if the status code is non-zero.


#### Authentication ####

All communication between the client and server shall use HTTPS. The client must receive an API key by using the request_key service. This service authenticates the XSEDE user/group and project then, if successful, returns an API key to be used for all other requests. For more details see the section below for the Authentication service type.

---

## Service Types ##

Service Type       | URI 
------------------ | ---
Network Allocation | /allocations 
Authentication     | /auth 
Network Statistics | /stats

---

### Network Allocation ###

A network allocation is the reservation of priority network resources for a specified amount of time. 

Attributes    | Description
------------- | -----------
allocation_id | **string**<br>  ID uniquely representing this allocation
duration      | **integer**<br> How long the priority path is estimated to be used represented in seconds.
start_time    | **intger**<br>  When the allocation was set as 'active'. Represented as UNIX epoch time.
end_time      | **intger**<br>  When the allocation is to be set as 'complete'. Represented as UNIX epoch time. This should be start_time + duration.
state         | **string**<br>  Current status of the allocation. [ pending; active; complete ]
bandwidth     | **integer**<br> Target bandwidth desired by the requestor. CONGA can take this into consideration but bandwidth is not guarenteed.
user_id       | **string**<br>  Name of the XSEDE user or user group.
project_id    | **string**<br>  Name of the XSEDE project.
src_ip        | **string**<br>  IP address of the source host.
src_port      | **integer**<br> The source port number.
dst_ip        | **string**<br>  IP address of the destination host.
dst_port      | **integer**<br> The destination port number.

---

#### Create an allocation ####

Creates a new allocation request to be processed and scheduled by CONGA.

	POST /allocations

Arguments  | Description
---------- | -----------
api_key    | **required**<br>  Unique key given to an authenticated client
user_id    | **optional**<br>  Name of the XSEDE user or user group.
project_id | **required**<br>  Name of the XSEDE project.
src_ip     | **required**<br>  IP address of the source host.
src_port   | **optional**<br>  The source port number.
dst_ip     | **required**<br>  IP address of the destination host.
dst_port   | **optional**<br>  The destination port number.
duration   | **required**<br>  How long the priority path is estimated to be used.
bandwidth  | **optional**<br>  Target bandwidth desired by the requestor. CONGA can take this into consideration but bandwidth is not guarenteed.

**Request Example**
```
$ curl -X POST -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/allocations"
	-d '{
		"api_key": "[key]",
		"project_id": "project1",
		"src_ip": "192.127.1.10",
		"src_port": 4321,
		"dst_ip": "192.127.1.11",
		"dst_port": 3030,
		"duration": 3600
	}'
```

**Response Example**
```
{
	"status": 0,
	"results": [
		{
			"allocation_id": "[id]",
			"state": "pending"
		}
	],
	"errors": []
}
```

---

#### Retrieve allocation details ####

Returns all details known about the allocation.

	GET /allocations/{allocation_id}

Arguments | Description
--------- | -----------
api_key   | **required**<br>  Unique key given to an authenticated client


**Request Example**
```
$ curl -X GET -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/allocations/[allocations_id]"
	-d '{
		"api_key": "[key]"
	}'
```

**Response Example**
```
{
	"status": 0,
	"results": [
		{
			"allocation_id": "[id]",
			"start_time": 1429206574,
			"end_time": 1429210174,
			"state": "active",
			"bandwidth": null,
			"user_id": "abc123",
			"project_id": "projectB",
			"src_ip": "192.127.1.10",
			"src_port": null,
			"dst_ip": "192.127.1.11",
			"dst_port": null,
			"duration": 3600
		}
	],
	"errors": []
}
```

---

#### Update an allocation ####

All arguments are optional excluding api_key. Any valid arguments that are provided will be updated.

	POST /allocations/{allocation_id}

Arguments  | Description
---------- | -----------
api_key    | **required**<br>  Unique key given to an authenticated client
project_id | **optional**<br>  Name of the XSEDE project.
src_ip     | **optional**<br>  IP address of the source host.
src_port   | **optional**<br>  The source port number.
dst_ip     | **optional**<br>  IP address of the destination host.
dst_port   | **optional**<br>  The destination port number.
duration   | **optional**<br>  How long the priority path is estimated to be used.
bandwidth  | **optional**<br>  Target bandwidth desired by the requestor. CONGA can take this into consideration but bandwidth is not guarenteed.


**Request Example**
```
$ curl -X POST -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/allocations/[allocation_id]"
	-d '{
		"api_key": "[key]",
		"dst_ip": "192.127.1.22",
		"duration": 7200
	}'
```

**Response Example**
```
{
	"status": 0,
	"results": [
		{
			"allocation_id": "[id]",
			"start_time": 1429206574,
			"end_time": 1429213774,
			"state": "active",
			"bandwidth": null,
			"user_id": "abc123",
			"project_id": "projectB",
			"src_ip": "192.127.1.10",
			"src_port": null,
			"dst_ip": "192.127.1.22",
			"dst_port": null,
			"duration": 7200
		}
	],
	"errors": []

```

---

#### Delete an allocation ####

Removes an existing allocation request. 

	DELETE /allocations/{allocation_id}

Arguments | Description
--------- | -----------
api_key   | **required**<br>  Unique key given to an authenticated client

**Request Example**
```
$ curl -X DELETE -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/allocations/[allocation_id]"
	-d '{
		"api_key": "[key]"
	}'
```

**Response Example**
```
{
	"status": 0,
	"results": [
			"allocation_id": "[id]"
		],
	"errors": []
}
```

---

#### List all allocations ####

Lists all allocations for the given project.

	GET /allocations

Arguments  | Description
---------- | -----------
api_key    | **required**<br>  Unique key given to an authenticated client
project_id | **required**<br>  Name of XSEDE project or awarding grant

**Request Example**
```
$ curl -X GET -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/allocations"
	-d '{
		"api_key": "[key]",
		"project_id": "projectB"
	}'
```

**Response Example**
```
{
	"status": 0,
	"results": [
		{
			"allocation_id": "[id]",
			"start_time": 1429206574,
			"end_time": 1429213774,
			"state": "active",
			"bandwidth": null,
			"user_id": "abc123",
			"project_id": "projectB",
			"src_ip": "192.127.1.10",
			"src_port": null,
			"dst_ip": "192.127.1.22",
			"dst_port": null,
			"duration": 7200
		},
		{
			"allocation_id": "[id]",
			"start_time": 1429214774,
			"end_time": 1429218374,
			"state": "pending",
			"bandwidth": null,
			"user_id": "xyz987",
			"project_id": "projectB",
			"src_ip": "192.127.1.40",
			"src_port": null,
			"dst_ip": "192.127.1.45",
			"dst_port": null,
			"duration": 3600
		}
	],
	"errors": []
}
```

---

### Authentication ###

Most service requests require an API key. You will need to request a key by issuing a POST to /auth along with your credentials. An authenticated client is represented by the attributes listed below.

Attributes | Description
---------- | -----------
api_key    | **string**<br>  Unique key for an authenticated client.
start_time | **string**<br>  UNIX epoch time representing when the key was first created.
end_time   | **integer**<br> UNIX epoch time representing when the key will expire.
user_id    | **string**<br>  Name of the XSEDE user or user group.
project_id | **string**<br>  Name of the XSEDE project or awarding grant.

---

#### Request API key ####

Returns an API key to be used for subsequent requests.

	POST /auth

Arguments | Description
--------- | -----------
user_id   | **required**<br>  Name of the XSEDE user or user group
project_id| **required**<br>  Name of XSEDE project or awarding grant

**Request Example**
```
$ curl -X POST -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/auth"
	-d '{
		"user_id": "abc123",
		"project_id": "projectB"
	}'
```

**Response Example**
```
{
	"status": 0,
	"results": [
			"api_key": "[key]",
			"start_time": 1429212515,
			"end_time": 1429298915,
			"user_id": "abc123",
			"project_id": "projectB"
		],
	"errors": []
}
```

---

#### Retrieve API key status ####

Returns details about the key. Useful to querying how long until it expires.

	GET /auth/{api_key}

**Request Example**
```
$ curl -X GET -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/auth/[api_key]"
```

**Response Example**
```
{
	"status": 0,
	"results":
		[
			{
				"api_key": "[api_key]",
				"start_time": 1429206574,
				"end_time": 1429242574,
				"user_id": "abc123",
				"project_id": "project B"
			}
		],
	"errors": []
}
```

---

#### Renew API key ####

Extends the expiration date of an existing key. If the key no longer exists a new one must be requested.

	POST /auth/{api_key}

Arguments | Description
--------- | -----------
user_id   | **required**<br>  Name of the XSEDE user or user group
project_id| **required**<br>  Name of XSEDE project or awarding grant

**Request Example**
```
$ curl -X POST -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/auth/[api_key]"
	-d '{
		"user_id": "abc123",
		"project_id": "projectB"
	}'
```

**Response Example**
```
{
	"status": 0,
	"results":
		[
			{
				"api_key": "[api_key]",
				"start_time": 1429206574,
				"end_time": 1429245574,
				"user_id": "abc123",
				"project_id": "project B"
			}
		],
	"errors": []
}

```

---

#### Delete API key ####

Removes an existing key.

	DELETE /auth/{api_key}

Arguments | Description
--------- | -----------
user_id   | **required**<br>  Name of the XSEDE user or user group
project_id| **required**<br>  Name of XSEDE project or awarding grant

**Request Example**
```
$ curl -X DELETE -H 'Content-Type: application/json' \
	"https://[CONGA-host]/api/auth/[api_key]"
	-d '{
		"user_id": "abc123",
		"project_id": "projectB"
	}'
```

**Response Example**
```
{
	"status": 0,
	"results":
		[
			{
				"api_key": "[api_key]"
			}
		],
	"errors": []

```

---

### Network Statistics ###
