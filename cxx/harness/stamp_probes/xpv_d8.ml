type z = int
module type S3 = sig val x : bool end
let f = function Some (Some (module M : S3)) -> 1 | _ -> 2
let () = ignore (f (Some (Some (module struct let x = false end))))
