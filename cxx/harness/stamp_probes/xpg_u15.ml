module type S = sig type t end
module M1 = struct type t = int end
module M2 = struct type t = bool end
let rec f (module M : S) = ()
let () = f (module M1)
