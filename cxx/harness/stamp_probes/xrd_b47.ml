module type Total = sig
    type t
    val compare: t -> t -> int
    module N : sig type s val x : s end
end
module type P = sig
    include Total
end
