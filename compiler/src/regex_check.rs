// Checking a regex pattern written as a literal (`regex.compile("...")`) at
// compile time, with the rules the runtime applies (lt_regex_translate and
// regcomp in runtime/std.h): what it would refuse at run time is a compile
// error at the literal instead.

// What's wrong with the pattern, and a hint; None if it's fine.
pub fn problem(pattern: &str) -> Option<(String, String)> {
    let b = pattern.as_bytes();
    let mut i = if pattern.starts_with("(?i)") { 4 } else { 0 };
    let mut in_class = false;
    let mut depth = 0i32;
    let err = |m: &str, h: &str| Some((format!("regex: {}", m), h.to_string()));
    while i < b.len() {
        let c = b[i];
        if c == b'\\' {
            if i + 1 >= b.len() {
                return err("a trailing backslash", "write `\\\\\\\\` for a backslash");
            }
            let n = b[i + 1];
            match n {
                b'b' | b'B' | b'A' | b'z' | b'Z' => {
                    return err("\\b and other anchors except ^ and $ aren't supported", "match the characters around the word, e.g. `(^|[^[:alnum:]_])word([^[:alnum:]_]|$)`");
                }
                b'p' | b'P' => {
                    return err("\\p{...} classes aren't supported", "use a POSIX class such as `[[:alpha:]]`, or list the characters");
                }
                b'D' | b'W' | b'S' if in_class => {
                    return err("\\D, \\W and \\S can't be used inside [...]", "use [^...] with \\d, \\w or \\s");
                }
                _ => {}
            }
            i += 2;
            continue;
        }
        if in_class {
            if c == b'[' && b.get(i + 1) == Some(&b':') {
                if let Some(end) = pattern[i..].find(":]") {
                    i += end + 2;
                    continue;
                }
            }
            if c == b']' {
                in_class = false;
            }
            i += 1;
            continue;
        }
        match c {
            b'*' | b'+' | b'?' | b'}' if b.get(i + 1) == Some(&b'?') => {
                return err("lazy quantifiers like *? aren't supported", "match up to a stop character instead: `<([^>]+)>` rather than `<(.+?)>`");
            }
            b'(' if b.get(i + 1) == Some(&b'?') => {
                return err("(?...) groups aren't supported, except (?i) at the start", "use a plain group `(...)`; groups[1] and up are the parenthesized groups");
            }
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth < 0 {
                    return err("parentheses not balanced", "a `)` has no `(`; `\\\\)` matches a parenthesis");
                }
            }
            b'[' => {
                in_class = true;
                if b.get(i + 1) == Some(&b'^') {
                    i += 1;
                }
                if b.get(i + 1) == Some(&b']') {
                    i += 1;
                }
            }
            _ => {}
        }
        i += 1;
    }
    if in_class {
        return err("brackets not balanced", "close the `[` with `]`; `\\\\[` matches a bracket");
    }
    if depth > 0 {
        return err("parentheses not balanced", "close the `(` with `)`; `\\\\(` matches a parenthesis");
    }
    None
}

#[cfg(test)]
mod tests {
    use super::problem;

    #[test]
    fn rules() {
        assert!(problem("(\\d+)-(\\d+)").is_none());
        assert!(problem("(?i)hello").is_none());
        assert!(problem("^[\\w.+-]+@[\\w-]+\\.[\\w.]+$").is_none());
        assert!(problem("[[:alpha:]]+[]a]").is_none());
        assert!(problem("a[*?]").is_none());
        assert!(problem("<(.+?)>").unwrap().0.contains("lazy"));
        assert!(problem("(?:a)").unwrap().0.contains("(?...)"));
        assert!(problem("\\bword").unwrap().0.contains("anchors"));
        assert!(problem("(unclosed").unwrap().0.contains("parentheses"));
        assert!(problem("a)").unwrap().0.contains("parentheses"));
        assert!(problem("[abc").unwrap().0.contains("brackets"));
        assert!(problem("[\\S]").unwrap().0.contains("inside"));
        assert!(problem("\\p{L}").is_some());
        assert!(problem("a\\").unwrap().0.contains("backslash"));
    }
}
