module Create(P: sig type t end) = struct
    class virtual agent (x : P.t) = object
        val mutable limit_ = x
        method virtual private event: int -> string
    end
end
module Basic = struct
    include Create(struct type t = int end)
end
