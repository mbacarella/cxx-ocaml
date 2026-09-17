type t = ..
module type S = sig type t += E end
module M1 : S = struct type t += E end
let f ?(opt = 1) x = match x with M1.E -> () | _ -> ()
