module type S = sig
  module M : sig end
  (** doc *)
  [@@alert foo "x"]
end
