type z = int
module type S3 = sig val x : bool end
let f = function Some (module M : S3) -> 1 | _ -> 2
let m = (module struct let x = false end : S3)
let () = ignore (f (Some m))
