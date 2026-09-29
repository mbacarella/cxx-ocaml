type z = int
module type P = sig type t type t1 end
type 'a typ = Pair of (module P with type t = 'a)
let f (type s) (t : s typ) = match t with
  | Pair p -> (p : (module P with type t = s))
