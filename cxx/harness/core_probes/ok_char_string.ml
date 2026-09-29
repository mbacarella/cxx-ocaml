let s = String.make 3 'x'
let b = Bytes.of_string s
let () = Bytes.set b 0 'y'
let t = Bytes.to_string b
