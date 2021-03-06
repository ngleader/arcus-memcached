package MemcachedTest;
use strict;
use IO::Socket::INET;
use IO::Socket::UNIX;
use Exporter 'import';
use Carp qw(croak);
use vars qw(@EXPORT);

# Instead of doing the substitution with Autoconf, we assume that
# cwd == builddir.
use Cwd;
my $builddir = getcwd;


@EXPORT = qw(new_memcached new_memcached_engine sleep
             mem_get_is mem_gets mem_gets_is mem_stats mem_cmd_val_is
             getattr_is lop_get_is sop_get_is bop_get_is bop_gbp_is bop_smget_is
             bop_ext_get_is bop_ext_smget_is
             stats_prefixes_is stats_noprefix_is stats_prefix_is
             supports_sasl free_port);

sub sleep {
    my $n = shift;
    select undef, undef, undef, $n;
}

sub mem_stats {
    my ($sock, $type) = @_;
    $type = $type ? " $type" : "";
    print $sock "stats$type\r\n";
    my $stats = {};
    while (<$sock>) {
        last if /^(\.|END)/;
        /^(STAT|ITEM) (\S+)\s+([^\r\n]+)/;
        #print " slabs: $_";
        $stats->{$2} = $3;
    }
    return $stats;
}

sub mem_get_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $key, $val, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    my $expect_flags = $opts->{flags} || 0;
    my $dval = defined $val ? "'$val'" : "<undef>";
    $msg ||= "$key == $dval";

    print $sock "get $key\r\n";
    if (! defined $val) {
        my $line = scalar <$sock>;
        if ($line =~ /^VALUE/) {
            $line .= scalar(<$sock>) . scalar(<$sock>);
        }
        Test::More::is($line, "END\r\n", $msg);
    } else {
        my $len = length($val);
        my $body = scalar(<$sock>);
        my $expected = "VALUE $key $expect_flags $len\r\n$val\r\nEND\r\n";
        if (!$body || $body =~ /^END/) {
            Test::More::is($body, $expected, $msg);
            return;
        }
        $body .= scalar(<$sock>) . scalar(<$sock>);
        Test::More::is($body, $expected, $msg);
    }
}

sub mem_gets {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $key) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;
    my $val;
    my $expect_flags = $opts->{flags} || 0;

    print $sock "gets $key\r\n";
    my $response = <$sock>;
    if ($response =~ /^END/) {
        return "NOT_FOUND";
    }
    else
    {
        $response =~ /VALUE (.*) (\d+) (\d+) (\d+)/;
        my $flags = $2;
        my $len = $3;
        my $identifier = $4;
        read $sock, $val , $len;
        # get the END
        $_ = <$sock>;
        $_ = <$sock>;

        return ($identifier,$val);
    }

}
sub mem_gets_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $identifier, $key, $val, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    my $expect_flags = $opts->{flags} || 0;
    my $dval = defined $val ? "'$val'" : "<undef>";
    $msg ||= "$key == $dval";

    print $sock "gets $key\r\n";
    if (! defined $val) {
        my $line = scalar <$sock>;
        if ($line =~ /^VALUE/) {
            $line .= scalar(<$sock>) . scalar(<$sock>);
        }
        Test::More::is($line, "END\r\n", $msg);
    } else {
        my $len = length($val);
        my $body = scalar(<$sock>);
        my $expected = "VALUE $key $expect_flags $len $identifier\r\n$val\r\nEND\r\n";
        if (!$body || $body =~ /^END/) {
            Test::More::is($body, $expected, $msg);
            return;
        }
        $body .= scalar(<$sock>) . scalar(<$sock>);
        Test::More::is($body, $expected, $msg);
    }
}

# COLLECTION: common
sub mem_cmd_val_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $cmd, $val, $rst, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "$cmd: $val";

    print $sock "$cmd\r\n$val\r\n";

    my $resp = "";
    my $line = scalar <$sock>;
    while ($line !~ /^END/) {
        $resp = $resp . (substr $line, 0, length($line)-2) . "\n";
        $line = scalar <$sock>;
    }
    $resp = $resp . (substr $line, 0, length($line)-2);
    Test::More::is("$resp", "$rst", $msg);
}

# COLLECTION
sub getattr_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $val, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    my $dval = defined $val ? "'$val'" : "<undef>";
    $msg ||= "getattr $args == $dval";

    print $sock "getattr $args\r\n";

    my $expected = $val;
    my @res_array = ();
    my $line = scalar <$sock>;
    while ($line =~ /^ATTR/) {
        push(@res_array, substr $line, 5, length($line)-7);
        $line = scalar <$sock>;
    }
    my $response = join(" ", @res_array);
    Test::More::is($response, $expected, $msg);
}

# COLLECTION
sub lop_get_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $flags, $ecount, $values, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    my $dval = defined $values ? "'$values'" : "<undef>";
    $msg ||= "lop get $args == $flags $ecount $dval";

    print $sock "lop get $args\r\n";

    my $expected_head = "VALUE $flags $ecount\r\n";
    my $expected_body = $values;

    my $response_head = scalar <$sock>;
    my @value_array = ();
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^DELETED/ and $line !~ /^DELETED_DROPPED/) {
        $vleng = substr $line, 0, index($line, ' ');
        $rleng = length($vleng) + 1;
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $response_body = join(",", @value_array);

    Test::More::is("$response_head $response_body", "$expected_head $expected_body", $msg);
}

# COLLECTION
sub sop_get_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $flags, $ecount, $values, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    my $dval = defined $values ? "'$values'" : "<undef>";
    $msg ||= "sop get $args == $flags $ecount $dval";

    print $sock "sop get $args\r\n";

    my $expected_head = "VALUE $flags $ecount\r\n";
    my $expected_body = join(",", sort(split(",", $values)));

    my $response_head = scalar <$sock>;
    my @value_array = ();
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^DELETED/ and $line !~ /^DELETED_DROPPED/) {
        $vleng = substr $line, 0, index($line, ' ');
        $rleng = length($vleng) + 1;
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $response_body = join(",", sort(@value_array));

    Test::More::is("$response_head $response_body", "$expected_head $expected_body", $msg);
}

# COLLECTION
sub bop_get_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $flags, $ecount, $ebkeys, $values, $tailstr, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "bop get $args == $flags $ecount bkeys data";

    print $sock "bop get $args\r\n";

    my $expected_head = "VALUE $flags $ecount\r\n";
    my $expected_bkey = $ebkeys;
    my $expected_body = $values;
    my $expected_tail = "$tailstr\r\n";

    my $response_head = scalar <$sock>;
    my @ebkey_array = ();
    my @value_array = ();
    my $ebkey;
    my $eflag;
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^TRIMMED/ and $line !~ /^DELETED/ and $line !~ /^DELETED_DROPPED/) {
        $ebkey = substr $line, 0, index($line,' ');
        $rleng = length($ebkey) + 1;
        $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($vleng) + 1;
        if ((substr $vleng , 0, 2) eq "0x") {
            $eflag = $vleng;
            $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
            $rleng = $rleng + length($vleng) + 1;
        }
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@ebkey_array, $ebkey);
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $response_bkey = join(",", @ebkey_array);
    my $response_body = join(",", @value_array);
    my $response_tail = $line;

    Test::More::is("$response_head $response_bkey $response_body $response_tail",
                   "$expected_head $expected_bkey $expected_body $expected_tail", $msg);
}

# COLLECTION
sub bop_ext_get_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $flags, $ecount, $ebkeys, $eflags, $values, $tailstr, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "bop get $args == $flags $ecount ebkeys eflags values";

    print $sock "bop get $args\r\n";

    my $expected_head = "VALUE $flags $ecount\r\n";
    my $expected_bkey = $ebkeys;
    my $expected_eflg = $eflags;
    my $expected_body = $values;
    my $expected_tail = "$tailstr\r\n";

    my $response_head = scalar <$sock>;
    my @ebkey_array = ();
    my @eflag_array = ();
    my @value_array = ();
    my $ebkey;
    my $eflag;
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^TRIMMED/ and $line !~ /^DELETED/ and $line !~ /^DELETED_DROPPED/) {
        $ebkey = substr $line, 0, index($line,' ');
        $rleng = length($ebkey) + 1;
        if ((substr $line, $rleng, 2) eq "0x") {
            $eflag = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
            $rleng = $rleng + length($eflag) + 1;
        } else {
            $eflag = "";
        }
        $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($vleng) + 1;
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@ebkey_array, $ebkey);
        push(@eflag_array, $eflag);
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $response_bkey = join(",", @ebkey_array);
    my $response_eflg = join(",", @eflag_array);
    my $response_body = join(",", @value_array);
    my $response_tail = $line;

    Test::More::is("$response_head $response_bkey $response_eflg $response_body $response_tail",
                   "$expected_head $expected_bkey $expected_eflg $expected_body $expected_tail", $msg);
}

sub bop_gbp_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $flags, $ecount, $ebkeys, $values, $tailstr, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "bop gbp $args == $flags $ecount bkeys data";

    print $sock "bop gbp $args\r\n";

    my $expected_head = "VALUE $flags $ecount\r\n";
    my $expected_bkey = $ebkeys;
    my $expected_body = $values;
    my $expected_tail = "$tailstr\r\n";

    my $response_head = scalar <$sock>;
    my @ebkey_array = ();
    my @value_array = ();
    my $ebkey;
    my $eflag;
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^TRIMMED/ and $line !~ /^DELETED/ and $line !~ /^DELETED_DROPPED/) {
        $ebkey = substr $line, 0, index($line,' ');
        $rleng = length($ebkey) + 1;
        $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($vleng) + 1;
        if ((substr $vleng , 0, 2) eq "0x") {
            $eflag = $vleng;
            $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
            $rleng = $rleng + length($vleng) + 1;
        }
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@ebkey_array, $ebkey);
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $response_bkey = join(",", @ebkey_array);
    my $response_body = join(",", @value_array);
    my $response_tail = $line;

    Test::More::is("$response_head $response_bkey $response_body $response_tail",
                   "$expected_head $expected_bkey $expected_body $expected_tail", $msg);
}

# COLLECTION
sub bop_smget_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $keystr, $ecount, $keys, $flags, $ebkeys, $values, $miss_kcnt, $miss_keys, $tailstr, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "bop smget $args keystr == $ecount {key flags bkey value} $miss_kcnt {missed key}";

    print $sock "bop smget $args\r\n$keystr\r\n";

    my $exp_elem_head = "VALUE $ecount\r\n";
    my $exp_elem_keys = $keys;
    my $exp_elem_flgs = $flags;
    my $exp_elem_bkey = $ebkeys;
    my $exp_elem_vals = $values;
    my $exp_mkey_head = "MISSED_KEYS $miss_kcnt\r\n";
    my $exp_mkey_vals = $miss_keys;
    my $exp_elem_tail = "$tailstr\r\n";

    my $res_elem_head = scalar <$sock>;
    my @itkey_array = ();
    my @itflg_array = ();
    my @ebkey_array = ();
    my @value_array = ();
    my $itkey;
    my $itflg;
    my $ebkey;
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^MISSED_KEYS/) {
        $itkey = substr $line, 0, index($line,' ');
        $rleng = length($itkey) + 1;
        $itflg = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($itflg) + 1;
        $ebkey = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($ebkey) + 1;
        $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($vleng) + 1;
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@itkey_array, $itkey);
        push(@itflg_array, $itflg);
        push(@ebkey_array, $ebkey);
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $res_elem_keys = join(",", @itkey_array);
    my $res_elem_flgs = join(",", @itflg_array);
    my $res_elem_bkey = join(",", @ebkey_array);
    my $res_elem_vals = join(",", @value_array);

    my $res_mkey_head = $line;
    my @mskey_array = ();
    my $mskey;
    $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^TRIMMED/ and $line !~ /^DUPLICATED/ and $line !~ /^DUPLICATED_TRIMMED/) {
        $mskey = substr $line, 0, length($line)-2;
        push(@mskey_array, $mskey);
        $line = scalar <$sock>;
    }
    my $res_mkey_vals = join(",", @mskey_array);
    my $res_elem_tail = $line;

    Test::More::is("$res_elem_head $res_elem_bkey $res_elem_vals $res_mkey_head $res_mkey_vals $res_elem_tail",
                   "$exp_elem_head $exp_elem_bkey $exp_elem_vals $exp_mkey_head $exp_mkey_vals $exp_elem_tail", $msg);
    if ($exp_elem_keys ne "") {
        Test::More::is("$res_elem_keys", "$exp_elem_keys", $msg);
    }
    if ($exp_elem_flgs ne "") {
        Test::More::is("$res_elem_flgs", "$exp_elem_flgs", $msg);
    }
}

# COLLECTION
sub bop_ext_smget_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $args, $keystr, $ecount, $keys, $flags, $ebkeys, $eflags, $values, $miss_kcnt, $miss_keys, $tailstr, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "bop smget $args keystr == $ecount {key flags bkey [eflag] value} $miss_kcnt {missed key}";

    print $sock "bop smget $args\r\n$keystr\r\n";

    my $exp_elem_head = "VALUE $ecount\r\n";
    my $exp_elem_keys = $keys;
    my $exp_elem_flgs = $flags;
    my $exp_elem_bkey = $ebkeys;
    my $exp_elem_eflg = $eflags;
    my $exp_elem_vals = $values;
    my $exp_mkey_head = "MISSED_KEYS $miss_kcnt\r\n";
    my $exp_mkey_vals = $miss_keys;
    my $exp_elem_tail = "$tailstr\r\n";

    my $res_elem_head = scalar <$sock>;
    my @itkey_array = ();
    my @itflg_array = ();
    my @ebkey_array = ();
    my @eflag_array = ();
    my @value_array = ();
    my $itkey;
    my $itflg;
    my $ebkey;
    my $eflag;
    my $vleng;
    my $value;
    my $rleng;
    my $line = scalar <$sock>;
    while ($line !~ /^MISSED_KEYS/) {
        $itkey = substr $line, 0, index($line,' ');
        $rleng = length($itkey) + 1;
        $itflg = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($itflg) + 1;
        $ebkey = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($ebkey) + 1;
        if ((substr $line, $rleng, 2) eq "0x") {
            $eflag = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
            $rleng = $rleng + length($eflag) + 1;
        } else {
            $eflag = "";
        }
        $vleng = substr $line, $rleng, index($line,' ',$rleng)-$rleng;
        $rleng = $rleng + length($vleng) + 1;
        $value = substr $line, $rleng, length($line)-$rleng-2;
        push(@itkey_array, $itkey);
        push(@itflg_array, $itflg);
        push(@ebkey_array, $ebkey);
        push(@eflag_array, $eflag);
        push(@value_array, $value);
        $line  = scalar <$sock>;
    }
    my $res_elem_keys = join(",", @itkey_array);
    my $res_elem_flgs = join(",", @itflg_array);
    my $res_elem_bkey = join(",", @ebkey_array);
    my $res_elem_eflg = join(",", @eflag_array);
    my $res_elem_vals = join(",", @value_array);

    my $res_mkey_head = $line;
    my @mskey_array = ();
    my $mskey;
    $line = scalar <$sock>;
    while ($line !~ /^END/ and $line !~ /^TRIMMED/ and $line !~ /^DUPLICATED/ and $line !~ /^DUPLICATED_TRIMMED/) {
        $mskey = substr $line, 0, length($line)-2;
        push(@mskey_array, $mskey);
        $line = scalar <$sock>;
    }
    my $res_mkey_vals = join(",", @mskey_array);
    my $res_elem_tail = $line;

    Test::More::is("$res_elem_head $res_elem_bkey $res_elem_eflg $res_elem_vals $res_mkey_head $res_mkey_vals $res_elem_tail",
                   "$exp_elem_head $exp_elem_bkey $exp_elem_eflg $exp_elem_vals $exp_mkey_head $exp_mkey_vals $exp_elem_tail", $msg);
    if ($exp_elem_keys ne "") {
        Test::More::is("$res_elem_keys", "$exp_elem_keys", $msg);
    }
    if ($exp_elem_flgs ne "") {
        Test::More::is("$res_elem_flgs", "$exp_elem_flgs", $msg);
    }
}

# DELETE_BY_PREFIX
sub stats_prefixes_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $val, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "stats prefixes";

    print $sock "stats prefixes\r\n";

    my $expected = join(",", sort(split(",", $val)));
    my @res_array = ();
    my $line = scalar <$sock>;
    my $subline;

    while ($line !~ /^END/) {
        $subline = substr $line, 0, index($line, ' tsz');
        push(@res_array, $subline);
        $line = scalar <$sock>;
    }
    my $response = join(",", sort(@res_array));
    Test::More::is("$response $line", "$expected END\r\n", $msg);
}

# DELETE_BY_PREFIX
sub stats_noprefix_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $val, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "stats noprefix";

    print $sock "stats noprefix\r\n";
    my $expected = $val;
    my @res_array = ();
    my $line = scalar <$sock>;

    while ($line =~ /^PREFIX/) {
        my $subline = substr $line, 7, length($line) - 9;

        unless($subline =~ /^hash_items_bytes/ or $subline =~ /^name/) {
            push(@res_array, $subline);
        }

        $line = scalar <$sock>;
    }
    my $response = join(" ", @res_array);
    Test::More::is($response, $expected, $msg);
}

# DELETE_BY_PREFIX
sub stats_prefix_is {
    # works on single-line values only.  no newlines in value.
    my ($sock_opts, $prefix, $val, $msg) = @_;
    my $opts = ref $sock_opts eq "HASH" ? $sock_opts : {};
    my $sock = ref $sock_opts eq "HASH" ? $opts->{sock} : $sock_opts;

    $msg ||= "stats prefix $prefix";

    print $sock "stats prefix $prefix\r\n";
    my $expected = $val;
    my @res_array = ();
    my $line = scalar <$sock>;

    while ($line =~ /^PREFIX/) {
        my $subline = substr $line, 7, length($line) - 9;

        unless($subline =~ /^hash_items_bytes/ or $subline =~ /^name/ or $subline =~ /^tot_prefix_items/) {
            push(@res_array, $subline);
        }

        $line = scalar <$sock>;
    }
    my $response = join(" ", @res_array);
    Test::More::is($response, $expected, $msg);
}

sub free_port {
    my $type = shift || "tcp";
    my $sock;
    my $port;
    while (!$sock) {
        $port = int(rand(20000)) + 30000;
        $sock = IO::Socket::INET->new(LocalAddr => '127.0.0.1',
                                      LocalPort => $port,
                                      Proto     => $type,
                                      ReuseAddr => 1);
    }
    return $port;
}

sub supports_udp {
    my $output = `$builddir/memcached -h`;
    return 0 if $output =~ /^memcached 1\.1\./;
    return 1;
}

sub supports_sasl {
    my $output = `$builddir/memcached -h`;
    return 1 if $output =~ /sasl/i;
    return 0;
}

sub new_memcached {
    my ($args, $passed_port) = @_;
    my $port = $passed_port || free_port();
    my $host = '127.0.0.1';

    if ($ENV{T_MEMD_USE_DAEMON}) {
        my ($host, $port) = ($ENV{T_MEMD_USE_DAEMON} =~ m/^([^:]+):(\d+)$/);
        my $conn = IO::Socket::INET->new(PeerAddr => "$host:$port");
        if ($conn) {
            return Memcached::Handle->new(conn => $conn,
                                          host => $host,
                                          port => $port);
        }
        croak("Failed to connect to specified memcached server.") unless $conn;
    }

    my $udpport = free_port("udp");
    $args .= " -p $port";
    if (supports_udp()) {
        $args .= " -U $udpport";
    }
    if ($< == 0) {
        $args .= " -u root";
    }
    $args .= " -E $builddir/.libs/default_engine.so";

    my $childpid = fork();

    my $exe = "$builddir/memcached";
    croak("memcached binary doesn't exist.  Haven't run 'make' ?\n") unless -e $exe;
    croak("memcached binary not executable\n") unless -x _;

    unless ($childpid) {
        exec "$builddir/timedrun 600 $exe $args";
        exit; # never gets here.
    }

    # unix domain sockets
    if ($args =~ /-s (\S+)/) {
        sleep 1;
        my $filename = $1;
        my $conn = IO::Socket::UNIX->new(Peer => $filename) ||
            croak("Failed to connect to unix domain socket: $! '$filename'");

        return Memcached::Handle->new(pid  => $childpid,
                                      conn => $conn,
                                      domainsocket => $filename,
                                      host => $host,
                                      port => $port);
    }

    # try to connect / find open port, only if we're not using unix domain
    # sockets

    for (1..20) {
        my $conn = IO::Socket::INET->new(PeerAddr => "127.0.0.1:$port");
        if ($conn) {
            return Memcached::Handle->new(pid  => $childpid,
                                          conn => $conn,
                                          udpport => $udpport,
                                          host => $host,
                                          port => $port);
        }
        select undef, undef, undef, 0.10;
    }
    croak("Failed to startup/connect to memcached server.");
}

sub new_memcached_engine {
    my ($engine, $args, $passed_port) = @_;
    my $port = $passed_port || free_port();
    my $host = '127.0.0.1';

    if ($ENV{T_MEMD_USE_DAEMON}) {
        my ($host, $port) = ($ENV{T_MEMD_USE_DAEMON} =~ m/^([^:]+):(\d+)$/);
        my $conn = IO::Socket::INET->new(PeerAddr => "$host:$port");
        if ($conn) {
            return Memcached::Handle->new(conn => $conn,
                                          host => $host,
                                          port => $port);
        }
        croak("Failed to connect to specified memcached server.") unless $conn;
    }

    my $udpport = free_port("udp");
    $args .= " -p $port";
    if (supports_udp()) {
        $args .= " -U $udpport";
    }
    if ($< == 0) {
        $args .= " -u root";
    }
    $args .= " -E $builddir/.libs/$engine\_engine.so";

    my $childpid = fork();

    my $exe = "$builddir/memcached";
    croak("memcached binary doesn't exist.  Haven't run 'make' ?\n") unless -e $exe;
    croak("memcached binary not executable\n") unless -x _;

    unless ($childpid) {
        exec "$builddir/timedrun 600 $exe $args";
        exit; # never gets here.
    }

    # unix domain sockets
    if ($args =~ /-s (\S+)/) {
        sleep 1;
        my $filename = $1;
        my $conn = IO::Socket::UNIX->new(Peer => $filename) ||
            croak("Failed to connect to unix domain socket: $! '$filename'");

        return Memcached::Handle->new(pid  => $childpid,
                                      conn => $conn,
                                      domainsocket => $filename,
                                      host => $host,
                                      port => $port);
    }

    # try to connect / find open port, only if we're not using unix domain
    # sockets

    for (1..20) {
        my $conn = IO::Socket::INET->new(PeerAddr => "127.0.0.1:$port");
        if ($conn) {
            return Memcached::Handle->new(pid  => $childpid,
                                          conn => $conn,
                                          udpport => $udpport,
                                          host => $host,
                                          port => $port);
        }
        select undef, undef, undef, 0.10;
    }
    croak("Failed to startup/connect to memcached server.");
}

############################################################################
package Memcached::Handle;
sub new {
    my ($class, %params) = @_;
    return bless \%params, $class;
}

sub DESTROY {
    my $self = shift;
    kill 2, $self->{pid};
}

sub stop {
    my $self = shift;
    kill 15, $self->{pid};
}

sub host { $_[0]{host} }
sub port { $_[0]{port} }
sub udpport { $_[0]{udpport} }

sub sock {
    my $self = shift;

    if ($self->{conn} && ($self->{domainsocket} || getpeername($self->{conn}))) {
        return $self->{conn};
    }
    return $self->new_sock;
}

sub new_sock {
    my $self = shift;
    if ($self->{domainsocket}) {
        return IO::Socket::UNIX->new(Peer => $self->{domainsocket});
    } else {
        return IO::Socket::INET->new(PeerAddr => "$self->{host}:$self->{port}");
    }
}

sub new_udp_sock {
    my $self = shift;
    return IO::Socket::INET->new(PeerAddr => '127.0.0.1',
                                 PeerPort => $self->{udpport},
                                 Proto    => 'udp',
                                 LocalAddr => '127.0.0.1',
                                 LocalPort => MemcachedTest::free_port('udp'),
        );

}

1;
