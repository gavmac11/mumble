"""Exercise publication failures and retries without publishing anything to GitHub."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("publish_preview", Path(__file__).parents[1] / "publish-preview.py")
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dist = Path(self.temp.name)
        for name in publisher.ASSETS:
            (self.dist / name).write_bytes(name.encode())
        self.sha = "a" * 40
        self.ref = None
        self.release = None
        self.calls = []
        self.fail_upload = False
        self.mock = patch.object(publisher, "gh", side_effect=self.gh).start()
        self.addCleanup(patch.stopall)

    def gh(self, *args, **kwargs):
        self.calls.append(args)
        if args[:2] == ("api", "repos/test/repo/git/ref/tags/v1.7.123"):
            return json.dumps(self.ref) if self.ref else None
        if args[:2] == ("release", "view"):
            return json.dumps(self.release) if self.release else None
        if args[:3] == ("api", "--method", "POST"):
            self.ref = {"object": {"type": "commit", "sha": self.sha}}
        elif args[:2] == ("release", "create"):
            self.release = {"isDraft": True, "targetCommitish": self.sha}
        elif args[:2] == ("release", "upload") and self.fail_upload:
            raise RuntimeError("interrupted upload")
        elif args[:2] == ("release", "edit"):
            self.release["isDraft"] = False
        return ""

    def publish(self, version="1.7.123"):
        publisher.publish("test/repo", self.sha, version, self.dist, "https://example.com/run")

    def mutations(self):
        return [c for c in self.calls if c[:2] not in {
            ("api", "repos/test/repo/git/ref/tags/v1.7.123"), ("release", "view")}]

    def test_complete_release_is_published_after_upload(self):
        self.publish()
        mutations = self.mutations()
        self.assertEqual([c[:2] for c in mutations], [
            ("api", "--method"), ("release", "create"), ("release", "upload"), ("release", "edit")])
        self.assertIn("--draft", mutations[1])
        self.assertIn("--generate-notes", mutations[1])
        self.assertIn(self.sha, mutations[1])
        self.assertIn("--draft=false", mutations[-1])
        for name in publisher.ASSETS:
            self.assertIn(str(self.dist / name), mutations[2])
            digest = hashlib.sha256(name.encode()).hexdigest()
            self.assertIn(f"{digest}  {name}\n", (self.dist / "SHA256SUMS.txt").read_text())

    def test_upload_failure_leaves_draft_and_rerun_finishes_it(self):
        self.fail_upload = True
        with self.assertRaisesRegex(RuntimeError, "interrupted upload"):
            self.publish()
        self.assertTrue(self.release["isDraft"])
        self.fail_upload = False
        self.calls.clear()
        self.publish()
        self.assertEqual([c[:2] for c in self.mutations()], [("release", "upload"), ("release", "edit")])
        self.assertFalse(self.release["isDraft"])

    def test_published_release_is_never_overwritten(self):
        self.publish()
        self.calls.clear()
        self.publish()
        self.assertEqual(self.mutations(), [])

    def test_tag_collision_fails_before_mutation(self):
        self.ref = {"object": {"type": "commit", "sha": "b" * 40}}
        with self.assertRaisesRegex(ValueError, "different commit"):
            self.publish()
        self.assertEqual(self.mutations(), [])

    def test_draft_collision_fails_before_mutation(self):
        self.release = {"isDraft": True, "targetCommitish": "b" * 40}
        with self.assertRaisesRegex(ValueError, "different commit"):
            self.publish()
        self.assertEqual(self.mutations(), [])

    def test_missing_package_prevents_all_api_calls(self):
        (self.dist / publisher.ASSETS[-1]).unlink()
        with self.assertRaisesRegex(ValueError, "Missing or empty"):
            self.publish()
        self.assertEqual(self.calls, [])

    def test_version_overflow_prevents_all_api_calls(self):
        with self.assertRaisesRegex(ValueError, "limit"):
            self.publish("1.7.65536")
        self.assertEqual(self.calls, [])


class GitHubErrors(unittest.TestCase):
    def test_auth_and_network_errors_are_not_treated_as_missing(self):
        for error in ["HTTP 403 Forbidden", "connection reset", "HTTP 500"]:
            with patch.object(publisher.subprocess, "run") as run:
                run.return_value.returncode = 1
                run.return_value.stderr = error
                with self.assertRaises(RuntimeError):
                    publisher.gh("api", "repos/test/repo", missing="HTTP 404")


if __name__ == "__main__":
    unittest.main()
