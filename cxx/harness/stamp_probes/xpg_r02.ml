module type S = sig type t end
module M1 = struct type t = int end
let f (module M : S) = ()
let y = f (module M1)
