let f x = if x < 0 then raise Not_found else x
let g x = try f x with Not_found -> 0 | Failure s -> String.length s
let h () = failwith "x"
let e = Invalid_argument "x"
