#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2003-2009 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(vlt => 1);

compile(
    verilator_flags2 => [ "--binary   --coverage  --timing -Wall" ],
    );

execute(
    all_run_flags => [" +verilator+coverage+file+$Self->{obj_dir}/coverage.dat"],
    check_finished => 1,
    );

# Read the input .v file and do any CHECK_COVER requests
inline_checks();

run(cmd => ["../bin/verilator_coverage",
            "--annotate-points",
            "--annotate", "$Self->{obj_dir}/annotated",
            "$Self->{obj_dir}/coverage.dat",
            ],
    verilator_run => 1,
    ) if !$Self->errors && !$Self->skips;

files_identical("$Self->{obj_dir}/annotated/t_cover_else_points.v", $Self->{golden_filename});

ok(1);
1;