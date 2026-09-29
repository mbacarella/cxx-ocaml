module type S = sig type t val x : t end
module M : S = struct type t = int let x = 1 end
let y = M.x
