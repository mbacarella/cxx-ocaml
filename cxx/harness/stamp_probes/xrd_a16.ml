module Basic = struct
    class virtual agent (x : int) = object
        val mutable limit_ = x
        method virtual private event: int -> string
    end
end
class virtual basic_agent x = object
    inherit Basic.agent x
end
