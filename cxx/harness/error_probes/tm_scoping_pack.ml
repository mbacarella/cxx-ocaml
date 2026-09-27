module type S = sig type t val x : t end
let f () = let module M = struct type t = A let x = A end in (module M : S with type t = M.t)
