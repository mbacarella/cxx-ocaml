module type S = sig type t end
module M1 = struct type t = int end
module M2 = struct type t = bool end
let f (module M : S) = ()
let g = f
let () = g (module M1)
