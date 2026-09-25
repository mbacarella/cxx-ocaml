module type S = sig type t val x : t end
module M : S = struct type t = int let x = 1 end
module N = struct let y = [M.x] end
