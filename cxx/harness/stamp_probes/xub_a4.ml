module type S = sig
  type t = T of int
  type u = { x : int }
  type v = V of int * int
  type w = { a : float; b : float }
end
module M : S = struct
  type t = T of int
  type u = { x : int }
  type v = V of int * int
  type w = { a : float; b : float }
end
module N = struct type t = { x : float } type u = { y : t } end
include (struct type z = Z of int type r = { f : Float.t } end)
type p = (module S)
type q = M.t = T of int
type nonrec s = { m : N.t }
