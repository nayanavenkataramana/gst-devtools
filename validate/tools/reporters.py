#!/usr/bin/python
#
# Copyright (c) 2013,Thibault Saunier <thibault.saunier@collabora.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this program; if not, write to the
# Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
# Boston, MA 02110-1301, USA.

""" Test Reporters implementation. """

import os
import re
import codecs
from loggable import Loggable
from xml.sax import saxutils
from utils import mkdir, Result, printc

UNICODE_STRINGS = (type(unicode()) == type(str()))


class UnknownResult(Exception):
    pass


CONTROL_CHARACTERS = re.compile(r"[\000-\010\013\014\016-\037]")


def xml_safe(value):
    """Replaces invalid XML characters with '?'."""
    return CONTROL_CHARACTERS.sub('?', value)


def escape_cdata(cdata):
    """Escape a string for an XML CDATA section."""
    return xml_safe(cdata).replace(']]>', ']]>]]&gt;<![CDATA[')


class Reporter(Loggable):
    name = 'simple'

    def __init__(self, options):
        Loggable.__init__(self)

        self._current_test = None
        self.out = None
        self.options = options
        self.stats = {'timeout': 0,
                      'failures': 0,
                      'passes': 0,
                      'skipped': 0
                      }
        self.results = []

    def before_test(self, test):
        """Initialize a timer before starting a test."""
        path = os.path.join(self.options.logsdir,
                            test.classname.replace(".", os.sep))
        mkdir(os.path.dirname(path))
        self.out = open(path, 'w+')
        self._current_test = test

    def set_failed(self, test):
        self.stats["failed"] += 1

    def set_passed(self, test):
        self.stats["passed"] += 1

    def add_results(self, test):
        self.debug("%s", test)
        if test.result == Result.PASSED:
            self.set_passed(test)
        elif test.result == Result.FAILED or \
                test.result == Result.TIMEOUT:
            self.set_failed(test)
        else:
            raise UnknownResult("%s" % test.result)

    def after_test(self):
        self.results.append(self._current_test)
        self.add_results(self._current_test)
        self.out.close()
        self.out = None
        self._current_test = None

    def final_report(self):
        for test in self.results:
            printc(test)


class XunitReporter(Reporter):
    """This reporter provides test results in the standard XUnit XML format."""
    name = 'xunit'
    encoding = 'UTF-8'
    xml_file = None

    def __init__(self, options):
        super(XunitReporter, self).__init__(options)
        self.errorlist = []

    def final_report(self):
        self.report()
        super(XunitReporter, self).final_report()

    def _get_captured(self):
        if self.out:
            self.out.seek(0)
            value = self.out.read()
            if value:
                return '<system-out><![CDATA[%s]]></system-out>' % \
                    escape_cdata(value)
        return ''

    def _quoteattr(self, attr):
        """Escape an XML attribute. Value can be unicode."""
        attr = xml_safe(attr)
        if isinstance(attr, unicode) and not UNICODE_STRINGS:
            attr = attr.encode(self.encoding)
        return saxutils.quoteattr(attr)

    def report(self):
        """Writes an Xunit-formatted XML file

        The file includes a report of test errors and failures.

        """
        self.debug("Writing XML file to: %s", self.options.xunit_file)
        self.xml_file = codecs.open(self.options.xunit_file, 'w',
                                    self.encoding, 'replace')
        self.stats['encoding'] = self.encoding
        self.stats['total'] = (self.stats['timeout'] + self.stats['failures']
                               + self.stats['passes'] + self.stats['skipped'])
        self.xml_file.write( u'<?xml version="1.0" encoding="%(encoding)s"?>'
            u'<testsuite name="gesprojectslauncher" tests="%(total)d" '
            u'errors="%(timeout)d" failures="%(failures)d" '
            u'skip="%(skipped)d">' % self.stats)
        self.xml_file.write(u''.join([self._forceUnicode(e)
                            for e in self.errorlist]))
        self.xml_file.write(u'</testsuite>')
        self.xml_file.close()

    def set_failed(self, test):
        """Add failure output to Xunit report.
        """
        self.stats['failures'] += 1
        self.errorlist.append(
            '<testcase classname=%(cls)s name=%(name)s time="%(taken).3f">'
            '<failure type=%(errtype)s message=%(message)s>'
            '</failure>%(systemout)s</testcase>' %
            {'cls': self._quoteattr(test.classname),
             'name': self._quoteattr(test.classname.split('.')[-1]),
             'taken': test.time_taken,
             'errtype': self._quoteattr(test.result),
             'message': self._quoteattr(test.message),
             'systemout': self._get_captured(),
             })

    def set_passed(self, test):
        """Add success output to Xunit report.
        """
        self.stats['passes'] += 1
        self.errorlist.append(
            '<testcase classname=%(cls)s name=%(name)s '
            'time="%(taken).3f">%(systemout)s</testcase>' %
            {'cls': self._quoteattr(test.classname),
             'name': self._quoteattr(test.classname.split('.')[-1]),
             'taken': test.time_taken,
             'systemout': self._get_captured(),
             })

    def _forceUnicode(self, s):
        if not UNICODE_STRINGS:
            if isinstance(s, str):
                s = s.decode(self.encoding, 'replace')
        return s
