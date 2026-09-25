module A = struct type t = int let x = 1 end
module type S = sig type t val x : t end
module B : S = A
