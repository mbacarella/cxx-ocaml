let k = Printf.ksprintf (fun s -> String.length s) "%s-%s"
let v = k "a" "b"
let p = Printf.sprintf "%5.2f|%-3d|%x|%S|%B" 1.0 2 3 "s" true
let q = Printf.sprintf "%%"
