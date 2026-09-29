type z = int
module type S3 = sig val x : bool end
let f (x : (module S3) option) = 1
let () = ignore (f (Some (module struct let x = false end)))
