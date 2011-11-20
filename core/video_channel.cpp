/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#include "StdAfx.h"

#include "video_channel.h"

#include "video_format.h"

#include "consumer/output.h"
#include "mixer/mixer.h"
#include "mixer/gpu/ogl_device.h"
#include "producer/stage.h"

#include <common/diagnostics/graph.h>

#include <string>

namespace caspar { namespace core {

struct video_channel::implementation : boost::noncopyable
{
	const int						index_;
	const video_format_desc			format_desc_;
	const safe_ptr<ogl_device>		ogl_;
	safe_ptr<diagnostics::graph>	graph_;

	safe_ptr<caspar::core::output>	output_;
	safe_ptr<caspar::core::mixer>	mixer_;
	safe_ptr<caspar::core::stage>	stage_;
	
public:
	implementation(int index, const video_format_desc& format_desc, const safe_ptr<ogl_device>& ogl)  
		: index_(index)
		, format_desc_(format_desc)
		, ogl_(ogl)
		, output_(new caspar::core::output(graph_, format_desc))
		, mixer_(new caspar::core::mixer(graph_, output_, format_desc, ogl))
		, stage_(new caspar::core::stage(graph_, mixer_, format_desc))	
	{
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		CASPAR_LOG(info) << print() << " Successfully Initialized.";
	}
	
	void set_video_format_desc(const video_format_desc& format_desc)
	{
		mixer_->set_video_format_desc(format_desc);
		output_->set_video_format_desc(format_desc);
		ogl_->gc();
	}
		
	std::wstring print() const
	{
		return L"video_channel[" + boost::lexical_cast<std::wstring>(index_+1) + L"|" +  format_desc_.name + L"]";
	}
};

video_channel::video_channel(int index, const video_format_desc& format_desc, const safe_ptr<ogl_device>& ogl) : impl_(new implementation(index, format_desc, ogl)){}
safe_ptr<stage> video_channel::stage() { return impl_->stage_;} 
safe_ptr<mixer> video_channel::mixer() { return impl_->mixer_;} 
safe_ptr<output> video_channel::output() { return impl_->output_;} 
video_format_desc video_channel::get_video_format_desc() const{return impl_->format_desc_;}
void video_channel::set_video_format_desc(const video_format_desc& format_desc){impl_->set_video_format_desc(format_desc);}

}}