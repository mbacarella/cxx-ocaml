module Create(P: sig type t end) = struct
    class virtual agent (x : P.t) = object
        val mutable limit_ = x
        method virtual private event: int -> string
    end
end
module Basic = Create(struct type t = int end)
class virtual basic_agent x = object
    inherit Basic.agent x
end
