module type S = sig type t end
module M1 = struct type t = int end
module M2 = struct type t = bool end
module N = struct let f (module M : S) = () end
let () = N.f (module M1)
