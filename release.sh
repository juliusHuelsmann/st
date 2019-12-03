set -e
# Scrpt for automatically exporting diff in the correct format that
# simultaneously applies and compiles the patch such that it can be checked.



# the commit onto which the patch will be applied, hence also the commit 
# marking the begin of the patch chunk.
mainlineCommit=$(git rev-parse --short $1)

# last commit that contains information on the patch that is to be exported 
# targetCommit=$2 #< not necessary, I assume that this commit is alwayws
# currently checked out, otherwise check it out here.

cdate=$(date "+%Y%m%d")
patchFile="st-vimBrowse-$cdate-$mainlineCommit.diff"

git format-patch --stdout $mainlineCommit > $patchFile

echo "output $patchFile"


git checkout raw 
if [[ -f "config.h" ]]; then 
  rm config.h; 
  echo "removed"
else 
  echo "does not exist"
fi
  echo "patching $patchFile"
patch -p1 < $patchFile
  echo "patched"

make -j5
  echo "built"

#git stash








