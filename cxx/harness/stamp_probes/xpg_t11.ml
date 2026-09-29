module type S = sig type t end
module M1 = struct type t = int end
module M2 = struct type t = bool end
let f ~m:(module M : S) = ()
let () = f ~m:(module M1)
