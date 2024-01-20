#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2003-2009 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(simulator => 1);

top_filename("t/t_cover_else_points.sv");
golden_filename("t/t_cover_else_points.out");
compile(
    verilator_flags2 => [ "--exe $Self->{t_dir}/$Self->{name}.cpp  --coverage  --timing -Wall" ],
    make_main =>0,
    );

#compile(
#    verilator_flags2 => [ "--exe --main  --coverage  --timing -Wall" ],
#    make_main =>0,
#    );

execute(
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
    );

files_identical("$Self->{obj_dir}/annotated/t_cover_else_points.sv", $Self->{golden_filename});

ok(1);
1;
