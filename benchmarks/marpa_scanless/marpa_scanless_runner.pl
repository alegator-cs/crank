#!/usr/bin/env perl
use 5.010001;
use strict;
use warnings;
use JSON::PP ();
use Digest::SHA qw(sha256_hex);
use Time::HiRes qw(clock_gettime clock_getres CLOCK_MONOTONIC CLOCK_PROCESS_CPUTIME_ID);

# No compile-time Marpa import: `perl -c` works when the dependency is absent.
# All grammar/input recognition is performed by Marpa's public Scanless API.
my $json = JSON::PP->new->canonical->allow_nonref;
my $receipt = {
    schema => 'crank-marpa-independent-v1',
    engine => 'marpa', frontend => 'Marpa::R2::Scanless',
    status => 'error', pid => $$, perl_version => "$^V",
    recognition => 'full-span Boolean membership via ROOT_DONE completion event',
    semantics => { status => 'not_run', scope => 'input recognizer calls made by harness',
        user_actions => 0, value_calls => 0,
        capture_outputs => 0, forest_enumeration => 0 },
    timers => {
        cpu => 'CLOCK_PROCESS_CPUTIME_ID', wall => 'CLOCK_MONOTONIC',
        unit => 'nanoseconds',
        cpu_resolution_ns => int(clock_getres(CLOCK_PROCESS_CPUTIME_ID) * 1e9 + 0.5),
        wall_resolution_ns => int(clock_getres(CLOCK_MONOTONIC) * 1e9 + 0.5),
    },
    phases => {}, warm_batches => [],
};

sub emit {
    my ($status, $exit, $reason) = @_;
    $receipt->{status} = $status;
    $receipt->{reason} = "$reason" if defined $reason;
    print $json->encode($receipt), "\n";
    exit $exit;
}

sub integer_arg {
    my ($value, $name, $min, $max) = @_;
    die "$name must be an integer in [$min,$max]\n"
        if !defined($value) || $value !~ /\A[0-9]+\z/ || $value < $min || $value > $max;
    return 0 + $value;
}

sub stamp { return [clock_gettime(CLOCK_PROCESS_CPUTIME_ID), clock_gettime(CLOCK_MONOTONIC)]; }
sub elapsed {
    my ($before, $after) = @_;
    return { cpu_ns => int(($after->[0] - $before->[0]) * 1e9 + 0.5),
        wall_ns => int(($after->[1] - $before->[1]) * 1e9 + 0.5) };
}

sub raw_file {
    my ($path, $limit) = @_;
    open my $fh, '<:raw', $path or die "Cannot open $path: $!\n";
    my $size = -s $fh;
    die "RESOURCE: file $path exceeds $limit bytes\n" if !defined($size) || $size > $limit;
    local $/;
    my $data = <$fh>;
    close $fh or die "Cannot close $path: $!\n";
    return defined($data) ? $data : '';
}

sub corpus_records {
    my ($blob) = @_;
    die "Bad corpus magic/header (expected CFI1)\n"
        if length($blob) < 8 || substr($blob, 0, 4) ne 'CFI1';
    my $n = unpack('V', substr($blob, 4, 4));
    die "RESOURCE: corpus has more than 1000000 records\n" if $n > 1_000_000;
    die "Corpus must have at least one record\n" if $n == 0;
    my $at = 8;
    my @rows;
    for my $index (0 .. $n - 1) {
        die "Truncated corpus record header $index\n" if $at + 5 > length($blob);
        my ($len, $expected) = unpack('VC', substr($blob, $at, 5));
        $at += 5;
        die "RESOURCE: input $index exceeds 16777216 bytes\n" if $len > 16_777_216;
        die "Invalid expected verdict at record $index\n" if $expected > 1;
        die "Truncated corpus input $index\n" if $at + $len > length($blob);
        my $input = substr($blob, $at, $len);
        $at += $len;
        die "UNSUPPORTED: non-ASCII corpus record $index\n" if $input =~ /[^\x00-\x7f]/;
        push @rows, { index => $index, input => $input, expected => $expected };
    }
    die "Trailing bytes after corpus records\n" if $at != length($blob);
    return \@rows;
}

sub contract_check {
    my ($dsl) = @_;
    die "UNSUPPORTED: grammar source must be ASCII\n" if $dsl =~ /[^\x00-\x7f]/;
    # These are contract lints, not a replacement grammar parser. Marpa parses DSL.
    die "UNSUPPORTED: required :start ::= START line missing\n"
        if $dsl !~ /^\s*:start\s*::=\s*START\s*$/m;
    die "UNSUPPORTED: ROOT_DONE completion declaration missing\n"
        if $dsl !~ /^\s*event\s+ROOT_DONE\s*=\s*completed\s+START\s*$/m;
    die "UNSUPPORTED: :discard invalidates strict byte-position contract\n"
        if $dsl =~ /^\s*:discard\b/m;
    my $wrapper_rules = 0;
    for my $line (split /\n/, $dsl) {
        next if $line =~ /^\s*#/;
        if ($line =~ /^\s*START\s*::=\s*([A-Za-z][A-Za-z0-9_]*)\s*$/) {
            ++$wrapper_rules;
            next;
        }
        if ($line =~ /^\s*[^:#].*?::=(.*)$/) {
            die "UNSUPPORTED: START must never appear on a G1 RHS\n" if $1 =~ /\bSTART\b/;
        }
    }
    die "UNSUPPORTED: START must have one single-symbol wrapper rule\n" if $wrapper_rules != 1;
}

sub recognize {
    my ($grammar, $input) = @_;
    my $len = length($input);
    my $result = { status => 'error', accepted => undef, read_calls => 0,
        resume_calls => 0, root_completion_events => 0, prefix_completion_events => 0,
        rejection_events => 0, exhaustion_events => 0, bytes => $len };
    my $slr;
    my $ok = eval {
        $slr = Marpa::R2::Scanless::R->new({
            grammar => $grammar, rejection => 'event', exhaustion => 'event',
        });
        ++$result->{read_calls};
        my $pos = $slr->read(\$input);
        my $steps = 0;
        my $previous;
        while (1) {
            die "No valid physical position returned by Scanless reader\n"
                if !defined($pos) || $pos < 0 || $pos > $len;
            my $root_at_end = 0;
            my $rejected = 0;
            for my $event (@{$slr->events()}) {
                my $name = $event->[0];
                if ($name eq 'ROOT_DONE') {
                    ++$result->{root_completion_events};
                    if ($pos == $len) { $root_at_end = 1; }
                    else { ++$result->{prefix_completion_events}; }
                } elsif ($name eq q{'rejected}) {
                    ++$result->{rejection_events};
                    $rejected = 1;
                } elsif ($name eq q{'exhausted}) {
                    ++$result->{exhaustion_events};
                } else {
                    die "UNSUPPORTED: unexpected parse event '$name'\n";
                }
            }
            $result->{end_position} = $pos;
            $result->{exhausted} = $slr->exhausted() ? JSON::PP::true : JSON::PP::false;
            if ($root_at_end && !$rejected) {
                $result->{accepted} = JSON::PP::true;
                $result->{status} = 'accepted';
                $result->{reason} = 'complete_start_at_full_input';
                last;
            }
            if ($rejected || $pos == $len || $result->{exhausted}) {
                $result->{accepted} = JSON::PP::false;
                $result->{status} = 'rejected';
                $result->{reason} = $rejected ? 'lexeme_rejection_event'
                    : $pos == $len ? 'eof_without_complete_start' : 'exhaustion_before_eof';
                last;
            }
            # A same-position rejection/exhaustion above is normal. Only a
            # same-position return with no terminal outcome is a loop error.
            die "Scanless reader failed to advance\n" if defined($previous) && $pos <= $previous;
            die "RESOURCE: reader event step budget exceeded\n" if ++$steps > $len + 2;
            $previous = $pos;
            ++$result->{resume_calls};
            $pos = $slr->resume();
        }
        1;
    };
    if (!$ok) {
        my $error = "$@";
        $result->{exception} = $error;
        # Source-reviewed R2 14 path for a byte absent from every L0 class.
        # Unexpected exceptions are never converted into negative membership.
        if ($error =~ /\AFailed at unacceptable character 0x[0-9a-fA-F]+(?:\s|\z)/) {
            $result->{status} = 'rejected';
            $result->{accepted} = JSON::PP::false;
            $result->{reason} = 'unacceptable_character_exception';
        } elsif ($error =~ /\ARESOURCE:/) { $result->{status} = 'resource'; }
        elsif ($error =~ /\AUNSUPPORTED:/) { $result->{status} = 'unsupported'; }
        else { $result->{status} = 'error'; }
    }
    # A fresh recognizer and its teardown are part of each recognition service.
    undef $slr;
    return $result;
}

# Timed service returns one Boolean scalar. Diagnostic hashes and counters above
# are reserved for untimed correctness probes. Error objects exist only on a
# failed path; unknown exceptions cannot masquerade as negative membership.
sub recognize_boolean {
    my ($grammar, $input) = @_;
    my $len = length($input);
    my ($slr, $accepted);
    my $ok = eval {
        $slr = Marpa::R2::Scanless::R->new({
            grammar => $grammar, rejection => 'event', exhaustion => 'event',
        });
        my $pos = $slr->read(\$input);
        my ($previous, $steps);
        $steps = 0;
        while (1) {
            die "No valid physical position returned by Scanless reader\n"
                if !defined($pos) || $pos < 0 || $pos > $len;
            my ($root_at_end, $rejected) = (0, 0);
            for my $event (@{$slr->events()}) {
                my $name = $event->[0];
                if ($name eq 'ROOT_DONE') { $root_at_end = 1 if $pos == $len; }
                elsif ($name eq q{'rejected}) { $rejected = 1; }
                elsif ($name ne q{'exhausted}) { die "UNSUPPORTED: unexpected parse event '$name'\n"; }
            }
            if ($root_at_end && !$rejected) { $accepted = 1; last; }
            if ($rejected || $pos == $len || $slr->exhausted()) { $accepted = 0; last; }
            die "Scanless reader failed to advance\n" if defined($previous) && $pos <= $previous;
            die "RESOURCE: reader event step budget exceeded\n" if ++$steps > $len + 2;
            $previous = $pos;
            $pos = $slr->resume();
        }
        1;
    };
    my $error = "$@";
    undef $slr; # Fresh recognizer teardown remains within timed service.
    return $accepted if $ok;
    return 0 if $error =~ /\AFailed at unacceptable character 0x[0-9a-fA-F]+(?:\s|\z)/;
    return { status => $error =~ /\ARESOURCE:/ ? 'resource'
        : $error =~ /\AUNSUPPORTED:/ ? 'unsupported' : 'error', exception => $error };
}

sub verdict_object {
    my ($value) = @_;
    return $value if ref $value;
    return { status => $value ? 'accepted' : 'rejected',
        accepted => $value ? JSON::PP::true : JSON::PP::false };
}

my ($label, $grammar_path, $corpus_path, $mode, $count, $samples, $target_ns);
my $args_ok = eval {
    die "Usage: $0 ENGINE GRAMMAR CORPUS_FILE check|measure COUNT SAMPLES TARGET_NS\n" if @ARGV != 7;
    ($label, $grammar_path, $corpus_path, $mode, $count, $samples, $target_ns) = @ARGV;
    die "UNSUPPORTED: ENGINE must be marpa or marpa-scanless\n"
        if $label ne 'marpa' && $label ne 'marpa-scanless';
    die "Mode must be check or measure\n" if $mode ne 'check' && $mode ne 'measure';
    $count = integer_arg($count, 'COUNT', $mode eq 'check' ? 0 : 1, 32_768);
    $samples = integer_arg($samples, 'SAMPLES', 1, 31);
    $target_ns = integer_arg($target_ns, 'TARGET_NS', 1, 60_000_000_000);
    1;
};
emit($@ =~ /\AUNSUPPORTED:/ ? 'unsupported' : 'error', 2, $@) if !$args_ok;
$receipt->{arguments} = { engine => $label, grammar => $grammar_path, corpus => $corpus_path,
    mode => $mode, count => $count, samples => $samples, target_ns => $target_ns };

my ($dsl, $rows, $corpus_sha);
my $load_ok = eval {
    $receipt->{runner_sha256} = sha256_hex(raw_file($0, 16_777_216));
    $dsl = raw_file($grammar_path, 16_777_216);
    my $blob = raw_file($corpus_path, 134_217_728);
    $corpus_sha = sha256_hex($blob);
    $rows = corpus_records($blob);
    contract_check($dsl);
    if ($mode eq 'measure') {
        my $needed = 1 + $count * $samples;
        die "RESOURCE: $needed corpus rows required; only " . scalar(@$rows) . " supplied\n"
            if $needed > @$rows;
        my %seen;
        for my $index (0 .. $needed - 1) {
            die "UNSUPPORTED: timed input content repeated at corpus row $index\n"
                if exists $seen{$rows->[$index]{input}};
            $seen{$rows->[$index]{input}} = 1;
        }
        $receipt->{fresh_content_unique} = JSON::PP::true;
        $receipt->{service_rows} = $count * $samples;
        $receipt->{unused_corpus_rows} = @$rows - $needed;
    }
    1;
};
if (!$load_ok) {
    my $error = "$@";
    emit($error =~ /\ARESOURCE:/ ? 'resource' : $error =~ /\AUNSUPPORTED:/ ? 'unsupported' : 'error', 2, $error);
}
$receipt->{grammar_sha256} = sha256_hex($dsl);
$receipt->{corpus_sha256} = $corpus_sha;
$receipt->{corpus_records} = scalar(@$rows);
$receipt->{source_bytes} = length($dsl);
$receipt->{contract} = { start => 'START', completed_event => 'ROOT_DONE',
    start_never_on_rhs => JSON::PP::true, root_nonnullable_required => JSON::PP::true,
    discard => JSON::PP::false, lexical_unit => 'one ASCII byte',
    validation => 'source lints plus independently specified grammar contract; runtime correctness pending' };

my $prep_begin = stamp();
my $module_ok = eval { require Marpa::R2; 1; };
my $module_end = stamp();
if (!$module_ok) {
    $receipt->{dependency} = 'Marpa::R2 with Scanless G/R and compiled XS Libmarpa';
    $receipt->{dependency_error} = "$@";
    $receipt->{benchmark_run} = JSON::PP::false;
    $receipt->{runtime_validation} = 'not_run_dependency_unavailable';
    # No valid preparation/recognition measurements on this path.
    emit('unavailable', 3, 'Marpa::R2 could not be loaded; no substitute frontend was used');
}
$receipt->{module_version} = "$Marpa::R2::VERSION";
$receipt->{module_path} = $INC{'Marpa/R2.pm'};
$receipt->{phases}{module_load} = elapsed($prep_begin, $module_end);
$receipt->{exception_classifier} = 'unacceptable-character allowlist from official R2 14.000000 source; validate installed version in check mode';

my $grammar;
my $prep_ok = eval { $grammar = Marpa::R2::Scanless::G->new({ source => \$dsl }); 1; };
my $prep_end = stamp();
$receipt->{phases}{public_grammar_preparation} = elapsed($module_end, $prep_end);
$receipt->{phases}{preparation_including_module_load} = elapsed($prep_begin, $prep_end);
emit('error', 4, "Grammar preparation failed: $@") if !$prep_ok;

my $first_begin = stamp();
my $first_value = recognize_boolean($grammar, $rows->[0]{input});
my $first_end = stamp();
my $first = verdict_object($first_value);
$receipt->{first} = { %$first, index => 0, expected => $rows->[0]{expected} ? JSON::PP::true : JSON::PP::false,
    %{elapsed($first_begin, $first_end)} };
$receipt->{phases}{cold_prepare_and_first_complete} = elapsed($prep_begin, $first_end);
$receipt->{first}{correct} = defined($first->{accepted}) && (!!$first->{accepted} == !!$rows->[0]{expected})
    ? JSON::PP::true : JSON::PP::false;
emit($first->{status}, 4, 'First input did not produce a Boolean verdict') if !defined($first->{accepted});
emit('error', 4, 'First input verdict differs from expected') if !$receipt->{first}{correct};

if ($mode eq 'check') {
    my $detailed_first = recognize($grammar, $rows->[0]{input});
    my $first_consistent = defined($detailed_first->{accepted})
        && !!$detailed_first->{accepted} == !!$first->{accepted};
    my @verdicts = ({ %$detailed_first, index => 0,
        expected => $rows->[0]{expected} ? JSON::PP::true : JSON::PP::false,
        lean_accepted => $first->{accepted}, lean_correct => $receipt->{first}{correct},
        lean_consistent => $first_consistent ? JSON::PP::true : JSON::PP::false,
        correct => $first_consistent ? JSON::PP::true : JSON::PP::false });
    my $mismatches = 0;
    ++$mismatches if !$first_consistent;
    for my $index (1 .. @$rows - 1) {
        my $lean = verdict_object(recognize_boolean($grammar, $rows->[$index]{input}));
        my $result = recognize($grammar, $rows->[$index]{input});
        my $lean_correct = defined($lean->{accepted}) && (!!$lean->{accepted} == !!$rows->[$index]{expected});
        my $consistent = defined($result->{accepted}) && defined($lean->{accepted})
            && !!$result->{accepted} == !!$lean->{accepted};
        my $correct = defined($result->{accepted}) && (!!$result->{accepted} == !!$rows->[$index]{expected})
            && $lean_correct && $consistent;
        ++$mismatches if !$correct;
        push @verdicts, { %$result, index => $index,
            expected => $rows->[$index]{expected} ? JSON::PP::true : JSON::PP::false,
            lean_accepted => $lean->{accepted}, lean_status => $lean->{status},
            lean_correct => $lean_correct ? JSON::PP::true : JSON::PP::false,
            lean_consistent => $consistent ? JSON::PP::true : JSON::PP::false,
            correct => $correct ? JSON::PP::true : JSON::PP::false };
    }
    $receipt->{verdicts} = \@verdicts;
    $receipt->{mismatches} = $mismatches;
    $receipt->{runtime_validation} = $mismatches ? 'failed' : 'passed_this_corpus';
    $receipt->{benchmark_run} = JSON::PP::false;
    emit($mismatches ? 'error' : 'ok', $mismatches ? 4 : 0,
        $mismatches ? 'Correctness corpus failed' : undef);
}

$receipt->{benchmark_run} = JSON::PP::true;
$receipt->{timed_diagnostics} = 'Boolean verdict only; detailed paths available from check-mode companion';
$receipt->{calibration} = { status => 'external_supervisor_required',
    policy => 'fixed COUNT; short batches flagged, never replay or wrap service inputs',
    included_in_warm_statistics => JSON::PP::false };
my $all_correct = 1;
my $all_long = 1;
for my $sample (0 .. $samples - 1) {
    my $from = 1 + $sample * $count;
    my @results;
    my $batch_begin = stamp();
    for my $index ($from .. $from + $count - 1) {
        push @results, recognize_boolean($grammar, $rows->[$index]{input});
    }
    my $batch_end = stamp();
    my $timing = elapsed($batch_begin, $batch_end);
    my %counts;
    my @failures;
    my $bytes = 0;
    for my $offset (0 .. $#results) {
        my $result = verdict_object($results[$offset]);
        my $index = $from + $offset;
        ++$counts{$result->{status}};
        $bytes += length($rows->[$index]{input});
        if (!defined($result->{accepted}) || (!!$result->{accepted} != !!$rows->[$index]{expected})) {
            $all_correct = 0;
            push @failures, { index => $index, expected => $rows->[$index]{expected}, %$result };
        }
    }
    my $long = $timing->{cpu_ns} >= $target_ns && $timing->{wall_ns} >= $target_ns;
    $all_long = 0 if !$long;
    push @{$receipt->{warm_batches}}, { sample => $sample, first_index => $from,
        count => $count, bytes => $bytes, %$timing,
        cpu_ns_per_input => $timing->{cpu_ns} / $count,
        wall_ns_per_input => $timing->{wall_ns} / $count,
        meets_target_cpu => $timing->{cpu_ns} >= $target_ns ? JSON::PP::true : JSON::PP::false,
        meets_target_wall => $timing->{wall_ns} >= $target_ns ? JSON::PP::true : JSON::PP::false,
        meets_target => $long ? JSON::PP::true : JSON::PP::false,
        outcome_counts => \%counts, failures => \@failures };
    last if !$all_correct;
}
$receipt->{all_batches_meet_target} = $all_long ? JSON::PP::true : JSON::PP::false;
$receipt->{runtime_validation} = $all_correct ? 'passed_measured_rows' : 'failed';
emit($all_correct ? 'ok' : 'error', $all_correct ? 0 : 4,
    $all_correct ? ($all_long ? undef : 'Short batches: increase COUNT using a separate calibration process and fresh corpus')
        : 'Measured input did not produce the expected Boolean verdict');
