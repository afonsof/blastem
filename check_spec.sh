#!/bin/bash

echo "=== CHECKING SPEC COMPLIANCE ==="
echo ""

# Check 1: freeze_seen field
echo "1. freeze_seen field in bsm_movie struct"
grep -n "freeze_seen" movie.c | head -1

# Check 2: unistd.h include guard
echo ""
echo "2. #include <unistd.h> with _WIN32 guard"
sed -n '8,10p' movie.c

# Check 3: movie_prepare_for_load resets
echo ""
echo "3. movie_prepare_for_load resets freeze_seen"
sed -n '242,245p' movie.c

# Check 4: movie_check_after_load logic
echo ""
echo "4. movie_check_after_load structure (reset in both branches)"
sed -n '247,258p' movie.c

# Check 5: movie_freeze section handling
echo ""
echo "5. movie_freeze: start_section, save ints, save buffer, end_section"
sed -n '260,271p' movie.c

# Check 6: movie_unfreeze load_buffer8 placement
echo ""
echo "6. movie_unfreeze: load_buffer8 only when recording"
sed -n '273,290p' movie.c

# Check 7: ftruncate guard
echo ""
echo "7. ftruncate guard with _WIN32"
sed -n '297,301p' movie.c

# Check 8: TESTMOVIEOBJS
echo ""
echo "8. TESTMOVIEOBJS includes serialize.o and util.o"
grep "^TESTMOVIEOBJS" Makefile

