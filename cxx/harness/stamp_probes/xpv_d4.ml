type z = int
module type S3 = sig val x : bool end
let f = function Some (module M : S3) -> 1 | _ -> 2
let () = ignore (f (Some (module struct let x = false end : S3)))
