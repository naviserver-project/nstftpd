# Author: Vlad Seryakov vlad@crystalballinc.com
# March 2006

namespace eval tftp {

}

ns_schedule_proc -once 0 tftp::init

# Global initialization
proc tftp::init {} {
}

# TFTP handler, should return error if access is denied,
# may return new file name that will be returned or
# return nothing to continue with requested file
proc tftp::server { op file ipaddr } {

    ns_log Notice tftp::server: $op $file $ipaddr
    
    switch -- $op {
     r {
       # Rewrite requested file with new name
       if { [string match "*/sip.cfg" $file] } {
         return [file dirname $file]/sip-$ipaddr.cfg
       }
     }
     
     w {
       # Default TFTP behaviour
       if { ![file exists $file] } { error "File should exist" }
     }
    }
    return
}
