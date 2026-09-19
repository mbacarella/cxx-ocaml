type z = int
module type S3 = sig val x : bool end
let f = function (module M : S3), 1 -> 1 | _ -> 2
let () = ignore (f ((module struct let x = false end), 1))
