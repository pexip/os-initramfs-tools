# Maintainer documentation for initramfs-tools

[[_TOC_]]

**NOTE:** The most recent version of this document is available at
docs/maintainer-notes.md in the [the git repository](#checkout)
or online at [salsa.debian.org](https://salsa.debian.org/kernel-team/initramfs-tools/blob/master/docs/maintainer-notes.md).

## <a name="definitions">1. Definitions</a>

| Name               | Meaning                   |
| ---                | ---                       |
| **`$mailaddress`** | mailaddress of the user   |
| **`$username`**    | name of the Salsa account |
| **`$version`**     | version string            |
| **`$yourname`**    | your fullname             |

## <a name="preparations">2. Preparations</a>

1. Install required software:

        # apt-get install git git-buildpackage dpkg-dev

1. Set environment variables (e.g. through your ~/.bashrc or ~/.zshrc) for devscripts (gbp dch):

        export DEBEMAIL=$mailaddress
        export DEBFULLNAME=$yourname

1. Set user name and email address for git (drop the --global option to use configuration per-repo basis):

        % git config --global user.name  "$yourname"
        % git config --global user.email "$mailaddress"

1. <a name="checkout">Checkout repository (anonymous):</a>

        % git clone https://salsa.debian.org/kernel-team/initramfs-tools.git
        % cd initramfs-tools

1. Checkout repository (with developer access):

        % git clone ssh://git@salsa.debian.org/kernel-team/initramfs-tools.git
        % cd initramfs-tools

## <a name="workflow">3. Workflow for daily work</a>

### <a name="newfeature">3.1 Implement new features</a>

1. Checkout new branch and switch to it:

        % git checkout -b $username/short-descr-of-new-feature

1. Hack and commit work:

        % $EDITOR $somefile
        % git add $somefile
        % git commit -s

   **NOTE:** Use 'Closes: #BUGID' for closing a bugreport and 'Thanks: Fullname
   &lt;mailaddress&gt;' for giving credits in your commit message. gbp dch will use
   this information for generating the changelog using the --meta option later
   on.

1. Finally push your branch to Salsa:

        % git push origin $username/short-descr-of-new-feature

### <a name="merge">3.2 Merge branches</a>

1. Switch to the branch you want to merge:

        % git checkout $username/new-feature

1. Rebase to master:

        % git rebase master

1. Switch to master branch and merge:

        % git checkout master
        % git merge $username/new-feature

1. Push:

        % git push

1. After branch is merged delete branch on server and locally:

        % git push origin :$username/short-descr-of-new-feature
        % git branch -d $username/short-descr-of-new-feature

1. If a branch is removed from the server it will stay locally. You can get of
any stale remote branches locally by executing:

        % git remote prune origin

### <a name="test">3.3 Test specific branch</a>

1. Checkout a specific branch iff branch is not already present locally:

        % git checkout -b somename/short-descr-of-new-feature origin/somename/short-descr-of-new-feature

1. Checkout a specific branch iff branch is already present locally:

        % git checkout -b somename/short-descr-of-new-feature

1. Adjust debian/changelog accordingly:

        % gbp dch --debian-branch="$(git branch | awk -F\*\   '/^* / { print $2}' )" \
          --since="v$(dpkg-parsechangelog | awk '/^Version:/ {print $2}')" -S --id-length=7 --meta --multimaint-merge

1. Build package:

        % gbp buildpackage --git-ignore-new --git-debian-branch="$(git branch | awk -F\*\  '/^* / { print $2}' )" --post-clean

### <a name="snapshot">3.4 Build snapshot version</a>

1. Adjust debian/changelog accordingly:

        % gbp dch --debian-branch="$(git branch | awk -F\*\   '/^* / { print $2}' )" \
          --since="v$(dpkg-parsechangelog | awk '/^Version:/ {print $2}')" -S --id-length=7 --meta --multimaint-merge

1. Build package:

        % gbp buildpackage --git-debian-branch="$(git branch | awk -F\*\  '/^* / { print $2}' )" --post-clean [-us -uc]

## <a name="contribute">4. Contribute</a>

1. Create patch:

        % git format-patch -s -p origin/master

1. Send patch file(s) to maintainers via mail (requires Debian package git-email):

        % git send-email --to=initramfs-tools@packages.debian.org $PATCHFILE[S]

1. The development mailinglists are [debian-kernel@lists.debian.org](mailto:debian-kernel@lists.debian.org)
   and [initramfs@vger.kernel.org](mailto:initramfs@vger.kernel.org).
   Discussion of features, bugs and patches are more than welcome on one
   of these lists.

## <a name="release">5. Release new version</a>

1. Creating changelog:

        % gbp dch --debian-branch master --release --since HASH

   or more dynamically:

        % gbp dch --meta --release --since v$(dpkg-parsechangelog | awk '/^Version:/ {print $2}') --debian-branch="$(git branch | awk -F\*\  '/^* / { print $2}' )" --id-length=7 --meta --multimaint-merge

   **NOTE:** we do not use history based sorting for the changelog entries but
   sort them by author.

1. Releasing:

        % git commit -a -s -m "Releasing version $version."

1. Tagging:

        % git tag -s v"$version" -m "release $version"

1. Pushing:

        % git push
        % git push --tags

1. Build in chroot and upload to ftp-master.

1. Send mail announcing the new initramfs-tools version with subject
   "initramfs-tools $VERSION release" to initramfs@vger.kernel.org,
   debian-kernel@lists.debian.org + kernel-team@lists.ubuntu.com - including a
   shortlog (generated through "git shortlog $TAG..").

## <a name="resources">6. Resources</a>

* [initramfs-tools git web interface](https://salsa.debian.org/kernel-team/initramfs-tools)
* [initramfs @ debian-wiki](https://wiki.debian.org/initramfs)
* [bugreports](https://bugs.debian.org/cgi-bin/pkgreport.cgi?src=initramfs-tools;dist=unstable)
* [initramfs-tools @ tracker](https://tracker.debian.org/pkg/initramfs-tools)
* [popcon graph](https://qa.debian.org/popcon.php?package=initramfs-tools)
* [bugreports @ ubuntu](https://bugs.launchpad.net/ubuntu/+source/initramfs-tools)
* [qa page @ ubuntu](http://status.qa.ubuntu.com/qapkgstatus/initramfs-tools)

## <a name="credits">7. Credits</a>

* Thanks to Daniel Baumann for his "[Debian Packaging with Git](https://web.archive.org/web/20110528125600/http://documentation.debian-projects.org/other/debian-packaging-git/)" which inspired this document.

## <a name="license">8. License</a>

* This document is licensed under GPL v2 or any later version.

*--  Michael Prokop &lt;[mika@debian.org](mailto:mika@debian.org)&gt;,
Ben Hutchings &lt;[benh@debian.org](mailto:benh@debian.org)&gt;*
