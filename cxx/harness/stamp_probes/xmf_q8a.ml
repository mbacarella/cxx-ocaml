module Make(P1 : sig type t end)(P2 : sig type t end) : sig type t
  end = struct type t = int end
