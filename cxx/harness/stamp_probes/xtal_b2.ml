module X = struct type t = int let compare = compare end
module Y = struct type t = bool let compare = compare end
module type T = sig
  val v : Set.Make(X).t
  val w : Set.Make(Y).t
end
