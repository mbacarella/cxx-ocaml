module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end

module Pr : Order.Total = struct type t = int let compare = compare end
type u = Pr.t
