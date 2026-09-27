module type ORDERED = sig type t val compare : t -> t -> int end
module MakeSet (O : ORDERED) : sig
  type t
  val empty : t
  val add : O.t -> t -> t
  val mem : O.t -> t -> bool
  val elements : t -> O.t list
end = struct
  type t = O.t list
  let empty = []
  let rec add x = function [] -> [x] | y :: r as l -> let c = O.compare x y in if c = 0 then l else if c < 0 then x :: l else y :: add x r
  let mem x l = List.exists (fun y -> O.compare x y = 0) l
  let elements l = l
end
module IntSet = MakeSet (Int)
module StrSet = MakeSet (struct type t = string let compare = String.compare end)
let packed : (module ORDERED with type t = int) = (module Int)
let sort_with (type a) (module O : ORDERED with type t = a) (l : a list) = List.sort O.compare l
