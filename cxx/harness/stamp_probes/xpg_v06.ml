type t = ..
module type S = sig type t += E end
module M1 : S = struct type t += E end
let f (module M : S) x = match x with M.E -> () | _ -> ()
