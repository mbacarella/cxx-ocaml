type t = ..
module type S = sig type t += E end
module M1 : S = struct type t += E end
module M2 : S = struct type t += E end
let binding1 (module M : S) ?(opt = M.E) () = opt
let x = binding1 (module M1) ();;
