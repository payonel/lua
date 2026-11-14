#!/bin/sh
# pom.sh ARTIFACT VERSION PACKAGING: the .pom of a package deploy-file
# publishes (deploy-file's own has no license)
cat <<POM
<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 https://maven.apache.org/xsd/maven-4.0.0.xsd">
  <modelVersion>4.0.0</modelVersion>
  <groupId>com.payonel</groupId>
  <artifactId>$1</artifactId>
  <version>$2</version>
  <packaging>$3</packaging>
  <url>https://github.com/payonel/lua</url>
  <licenses>
    <license>
      <name>MIT</name>
      <url>https://opensource.org/licenses/MIT</url>
    </license>
  </licenses>
  <scm>
    <url>https://github.com/payonel/lua</url>
  </scm>
</project>
POM
